"""Tests for the full Collection API: insert_one/insert_many, find_one/
find (filtering, dot-notation, comparison/logical operators,
projection, skip/limit), delete_one/delete_many, update_one/
update_many ($set/$unset/$inc, upsert), replace_one, find_one_and_
update/_delete/_replace, count_documents/estimated_document_count/
distinct, create_index/drop_index/list_indexes (including
index-accelerated queries, unique-constraint enforcement, and
compact()'s index rebuild), drop, compact, and cross-process-style
persistence (close + reopen), including the DIRTY-flag crash-recovery
path.
"""

from __future__ import annotations

import pytest

import custom_bson
from custom_bson import InsertOneResult, ObjectId
from custom_bson.exceptions import InvalidDocument


@pytest.fixture()
def client(tmp_path):
    c = custom_bson.MongoClient(str(tmp_path / "data"))
    yield c
    c.close()


@pytest.fixture()
def collection(client):
    return client.testdb.testcoll


# ---------------------------------------------------------------------------
# insert_one
# ---------------------------------------------------------------------------


def test_insert_one_generates_object_id(collection):
    result = collection.insert_one({"name": "Ada"})
    assert isinstance(result, InsertOneResult)
    assert isinstance(result.inserted_id, ObjectId)


def test_insert_one_preserves_supplied_id(collection):
    oid = ObjectId()
    result = collection.insert_one({"_id": oid, "name": "Ada"})
    assert result.inserted_id == oid


def test_insert_one_does_not_mutate_caller_document(collection):
    doc = {"name": "Ada"}
    collection.insert_one(doc)
    assert "_id" not in doc


# ---------------------------------------------------------------------------
# find_one / find
# ---------------------------------------------------------------------------


def test_find_one_no_match_returns_none(collection):
    assert collection.find_one({"name": "nobody"}) is None


def test_find_one_returns_matching_document(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    doc = collection.find_one({"name": "Ada"})
    assert doc["name"] == "Ada"
    assert doc["age"] == 36


def test_find_returns_all_matching_documents(collection):
    collection.insert_one({"tag": "x"})
    collection.insert_one({"tag": "x"})
    collection.insert_one({"tag": "y"})
    assert len(list(collection.find({"tag": "x"}))) == 2


def test_find_empty_filter_returns_everything(collection):
    collection.insert_one({"a": 1})
    collection.insert_one({"a": 2})
    assert len(list(collection.find({}))) == 2
    assert len(list(collection.find())) == 2


def test_find_dot_notation(collection):
    collection.insert_one({"user": {"name": "Ada", "address": {"city": "London"}}})
    assert collection.find_one({"user.name": "Ada"}) is not None
    assert collection.find_one({"user.address.city": "London"}) is not None
    assert collection.find_one({"user.address.city": "Paris"}) is None


def test_find_array_containment(collection):
    collection.insert_one({"tags": ["a", "b", "c"]})
    assert collection.find_one({"tags": "b"}) is not None
    assert collection.find_one({"tags": "z"}) is None


def test_find_comparison_operators(collection):
    collection.insert_one({"age": 20})
    collection.insert_one({"age": 30})
    collection.insert_one({"age": 40})
    assert len(list(collection.find({"age": {"$gt": 25}}))) == 2
    assert len(list(collection.find({"age": {"$gte": 30}}))) == 2
    assert len(list(collection.find({"age": {"$lt": 30}}))) == 1
    assert len(list(collection.find({"age": {"$lte": 30}}))) == 2
    assert len(list(collection.find({"age": {"$ne": 30}}))) == 2
    assert len(list(collection.find({"age": {"$in": [20, 40]}}))) == 2
    assert len(list(collection.find({"age": {"$nin": [20, 40]}}))) == 1


def test_find_not_operator(collection):
    collection.insert_one({"age": 20})
    collection.insert_one({"age": 40})
    docs = list(collection.find({"age": {"$not": {"$gt": 25}}}))
    assert [d["age"] for d in docs] == [20]


def test_find_and_or(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    collection.insert_one({"name": "Bo", "age": 20})
    collection.insert_one({"name": "Cy", "age": 45})
    assert len(list(collection.find({"$and": [{"age": {"$gte": 30}}, {"age": {"$lte": 40}}]}))) == 1
    assert len(list(collection.find({"$or": [{"name": "Ada"}, {"name": "Cy"}]}))) == 2


def test_find_bool_does_not_match_int_equality(collection):
    collection.insert_one({"flag": True})
    collection.insert_one({"count": 1})
    assert collection.find_one({"flag": 1}) is None
    assert collection.find_one({"count": True}) is None


def test_find_null_matches_missing_field(collection):
    collection.insert_one({"a": 1})  # present, non-null -- must not match
    collection.insert_one({"a": None})  # present, explicitly null -- must match
    collection.insert_one({"b": 1})  # "a" missing entirely -- must also match
    docs = list(collection.find({"a": None}))
    assert len(docs) == 2
    assert all(d.get("a") is None for d in docs)


def test_find_invalid_top_level_operator_raises(collection):
    with pytest.raises(InvalidDocument):
        list(collection.find({"$bogus": 1}))


def test_find_projection_inclusion(collection):
    collection.insert_one({"a": 1, "b": 2, "c": 3})
    doc = collection.find_one({}, projection={"a": 1})
    assert set(doc.keys()) == {"_id", "a"}


def test_find_projection_exclusion(collection):
    collection.insert_one({"a": 1, "b": 2, "c": 3})
    doc = collection.find_one({}, projection={"b": 0})
    assert "b" not in doc
    assert doc["a"] == 1 and doc["c"] == 3


def test_find_projection_mixed_raises(collection):
    with pytest.raises(ValueError):
        collection.find_one({}, projection={"a": 1, "b": 0})


def test_find_skip_and_limit(collection):
    for i in range(5):
        collection.insert_one({"i": i})
    docs = list(collection.find({}, skip=2, limit=2))
    assert len(docs) == 2
    assert [d["i"] for d in docs] == [2, 3]


def test_find_limit_zero_means_unlimited(collection):
    for i in range(3):
        collection.insert_one({"i": i})
    assert len(list(collection.find({}, limit=0))) == 3


# ---------------------------------------------------------------------------
# delete_one
# ---------------------------------------------------------------------------


def test_delete_one_removes_a_match(collection):
    collection.insert_one({"tag": "x"})
    collection.insert_one({"tag": "x"})
    result = collection.delete_one({"tag": "x"})
    assert result.deleted_count == 1
    assert len(list(collection.find({"tag": "x"}))) == 1


def test_delete_one_no_match(collection):
    result = collection.delete_one({"tag": "nope"})
    assert result.deleted_count == 0


def test_delete_one_then_find_one_returns_none(collection):
    collection.insert_one({"name": "Bo"})
    collection.delete_one({"name": "Bo"})
    assert collection.find_one({"name": "Bo"}) is None


def test_delete_many(collection):
    collection.insert_one({"tag": "x"})
    collection.insert_one({"tag": "x"})
    collection.insert_one({"tag": "y"})
    result = collection.delete_many({"tag": "x"})
    assert result.deleted_count == 2
    assert len(list(collection.find({}))) == 1


# ---------------------------------------------------------------------------
# update_one / update_many / $set /$unset / $inc / upsert
# ---------------------------------------------------------------------------


def test_update_one_set_and_inc(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    result = collection.update_one({"name": "Ada"}, {"$set": {"age": 37}, "$inc": {"visits": 1}})
    assert result.matched_count == 1
    assert result.modified_count == 1
    doc = collection.find_one({"name": "Ada"})
    assert doc["age"] == 37
    assert doc["visits"] == 1


def test_update_one_unset(collection):
    collection.insert_one({"name": "Ada", "temp": "x"})
    collection.update_one({"name": "Ada"}, {"$unset": {"temp": ""}})
    doc = collection.find_one({"name": "Ada"})
    assert "temp" not in doc


def test_update_one_dot_notation_set(collection):
    collection.insert_one({"name": "Ada", "address": {"city": "London"}})
    collection.update_one({"name": "Ada"}, {"$set": {"address.city": "Paris"}})
    doc = collection.find_one({"name": "Ada"})
    assert doc["address"]["city"] == "Paris"


def test_update_one_no_match(collection):
    result = collection.update_one({"name": "nobody"}, {"$set": {"a": 1}})
    assert result.matched_count == 0
    assert result.modified_count == 0
    assert result.upserted_id is None


def test_update_one_no_op_reports_zero_modified(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    result = collection.update_one({"name": "Ada"}, {"$set": {"age": 36}})
    assert result.matched_count == 1
    assert result.modified_count == 0


def test_update_one_rejects_raw_replacement_document(collection):
    collection.insert_one({"name": "Ada"})
    with pytest.raises(custom_bson.InvalidUpdateDocument):
        collection.update_one({"name": "Ada"}, {"age": 99})


def test_update_one_rejects_id_mutation(collection):
    collection.insert_one({"_id": ObjectId(), "name": "Ada"})
    with pytest.raises(InvalidDocument):
        collection.update_one({"name": "Ada"}, {"$set": {"_id": ObjectId()}})


def test_update_one_inc_on_missing_field_defaults_to_zero(collection):
    collection.insert_one({"name": "Ada"})
    collection.update_one({"name": "Ada"}, {"$inc": {"score": 5}})
    assert collection.find_one({"name": "Ada"})["score"] == 5


def test_update_one_inc_on_non_numeric_field_raises(collection):
    collection.insert_one({"name": "Ada", "score": "not a number"})
    with pytest.raises(InvalidDocument):
        collection.update_one({"name": "Ada"}, {"$inc": {"score": 1}})


def test_update_one_upsert_inserts_when_no_match(collection):
    result = collection.update_one({"name": "Zed"}, {"$set": {"age": 1}}, upsert=True)
    assert result.matched_count == 0
    assert result.modified_count == 0
    assert isinstance(result.upserted_id, ObjectId)
    doc = collection.find_one({"name": "Zed"})
    assert doc["name"] == "Zed" and doc["age"] == 1


def test_update_one_upsert_seeds_from_equality_filter(collection):
    collection.update_one({"name": "Zed", "kind": "user"}, {"$set": {"age": 1}}, upsert=True)
    doc = collection.find_one({"name": "Zed"})
    assert doc["kind"] == "user"


def test_update_one_no_upsert_when_disabled(collection):
    result = collection.update_one({"name": "Zed"}, {"$set": {"age": 1}}, upsert=False)
    assert result.matched_count == 0
    assert result.upserted_id is None
    assert collection.find_one({"name": "Zed"}) is None


def test_update_many_updates_every_match(collection):
    collection.insert_one({"tag": "x", "n": 1})
    collection.insert_one({"tag": "x", "n": 2})
    collection.insert_one({"tag": "y", "n": 3})
    result = collection.update_many({"tag": "x"}, {"$inc": {"n": 10}})
    assert result.matched_count == 2
    assert result.modified_count == 2
    values = sorted(d["n"] for d in collection.find({"tag": "x"}))
    assert values == [11, 12]


def test_update_many_upsert_when_no_match(collection):
    result = collection.update_many({"tag": "z"}, {"$set": {"n": 1}}, upsert=True)
    assert isinstance(result.upserted_id, ObjectId)
    assert collection.find_one({"tag": "z"}) is not None


# ---------------------------------------------------------------------------
# replace_one
# ---------------------------------------------------------------------------


def test_replace_one_preserves_id(collection):
    r = collection.insert_one({"name": "Ada", "age": 36})
    result = collection.replace_one({"name": "Ada"}, {"name": "Ada", "age": 99})
    assert result.matched_count == 1 and result.modified_count == 1
    doc = collection.find_one({"name": "Ada"})
    assert doc["_id"] == r.inserted_id
    assert doc["age"] == 99
    assert "name" in doc  # old fields not merged, but replacement itself has them


def test_replace_one_drops_fields_not_in_replacement(collection):
    collection.insert_one({"name": "Ada", "age": 36, "extra": "gone"})
    collection.replace_one({"name": "Ada"}, {"name": "Ada"})
    doc = collection.find_one({"name": "Ada"})
    assert "extra" not in doc and "age" not in doc


def test_replace_one_rejects_operator_document(collection):
    collection.insert_one({"name": "Ada"})
    with pytest.raises(InvalidDocument):
        collection.replace_one({"name": "Ada"}, {"$set": {"age": 1}})


def test_replace_one_rejects_id_change(collection):
    collection.insert_one({"_id": ObjectId(), "name": "Ada"})
    with pytest.raises(InvalidDocument):
        collection.replace_one({"name": "Ada"}, {"_id": ObjectId(), "name": "Ada"})


def test_replace_one_upsert(collection):
    result = collection.replace_one({"name": "Zed"}, {"name": "Zed", "age": 1}, upsert=True)
    assert isinstance(result.upserted_id, ObjectId)
    assert collection.find_one({"name": "Zed"}) is not None


# ---------------------------------------------------------------------------
# find_one_and_update / find_one_and_delete / find_one_and_replace
# ---------------------------------------------------------------------------


def test_find_one_and_update_returns_pre_image_by_default(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    doc = collection.find_one_and_update({"name": "Ada"}, {"$set": {"age": 37}})
    assert doc["age"] == 36
    assert collection.find_one({"name": "Ada"})["age"] == 37


def test_find_one_and_update_returns_post_image_when_requested(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    doc = collection.find_one_and_update({"name": "Ada"}, {"$set": {"age": 37}}, return_document=True)
    assert doc["age"] == 37


def test_find_one_and_update_no_match_returns_none(collection):
    assert collection.find_one_and_update({"name": "nobody"}, {"$set": {"a": 1}}) is None


def test_find_one_and_delete_returns_deleted_document(collection):
    collection.insert_one({"name": "Ada"})
    doc = collection.find_one_and_delete({"name": "Ada"})
    assert doc["name"] == "Ada"
    assert collection.find_one({"name": "Ada"}) is None


def test_find_one_and_delete_no_match_returns_none(collection):
    assert collection.find_one_and_delete({"name": "nobody"}) is None


def test_find_one_and_replace_returns_pre_image_by_default(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    doc = collection.find_one_and_replace({"name": "Ada"}, {"name": "Ada", "age": 99})
    assert doc["age"] == 36
    assert collection.find_one({"name": "Ada"})["age"] == 99


def test_find_one_and_replace_returns_post_image_when_requested(collection):
    collection.insert_one({"name": "Ada", "age": 36})
    doc = collection.find_one_and_replace(
        {"name": "Ada"}, {"name": "Ada", "age": 99}, return_document=True
    )
    assert doc["age"] == 99


# ---------------------------------------------------------------------------
# drop / compact / persistence
# ---------------------------------------------------------------------------


def test_drop_removes_backing_file(collection):
    collection.insert_one({"a": 1})
    file_path = collection._file_path
    assert __import__("os").path.exists(file_path)
    collection.drop()
    assert not __import__("os").path.exists(file_path)


def test_drop_removes_index_files(collection):
    import os

    collection.insert_one({"a": 1})
    collection.create_index("a")
    db_dir = os.path.dirname(collection._file_path)
    assert any(f.endswith(".bidx") for f in os.listdir(db_dir))
    collection.drop()
    assert not os.path.isdir(db_dir) or not any(f.endswith(".bidx") for f in os.listdir(db_dir))


def test_compact_preserves_live_documents_and_shrinks_tombstones(collection):
    for i in range(10):
        collection.insert_one({"i": i})
    for i in range(8):
        collection.delete_one({"i": i})
    before = sorted(d["i"] for d in collection.find({}))
    collection.compact()
    after = sorted(d["i"] for d in collection.find({}))
    assert before == after == [8, 9]


def test_compact_preserves_index_correctness(collection):
    collection.create_index("i", unique=True)
    for i in range(20):
        collection.insert_one({"i": i})
    for i in range(15):
        collection.delete_one({"i": i})

    before_indexed = sorted(d["i"] for d in collection.find({"i": {"$gte": 0}}))
    before_full_scan = sorted(d["i"] for d in collection.find({}))
    collection.compact()
    after_indexed = sorted(d["i"] for d in collection.find({"i": 17}))
    after_full_scan = sorted(d["i"] for d in collection.find({}))

    assert before_indexed == before_full_scan
    assert after_full_scan == list(range(15, 20))
    assert after_indexed == [17]
    # the index must still be usable and enforce uniqueness after compact
    with pytest.raises(custom_bson.DuplicateKeyError):
        collection.insert_one({"i": 17})
    # and still be listed under the same name
    assert [ix["name"] for ix in collection.list_indexes()] == ["i_1"]


def test_close_and_reopen_persists_data(tmp_path):
    data_dir = str(tmp_path / "data")
    client1 = custom_bson.MongoClient(data_dir)
    client1.testdb.testcoll.insert_one({"name": "Ada"})
    client1.close()

    client2 = custom_bson.MongoClient(data_dir)
    doc = client2.testdb.testcoll.find_one({"name": "Ada"})
    assert doc is not None and doc["name"] == "Ada"
    client2.close()


def test_dirty_flag_recovery_after_simulated_crash(tmp_path):
    data_dir = tmp_path / "data"
    client1 = custom_bson.MongoClient(str(data_dir))
    coll1 = client1.testdb.testcoll
    coll1.insert_one({"a": 1})
    coll1.insert_one({"a": 2})
    file_path = coll1._file_path
    data_end = coll1._handle.data_end()

    # Simulate a crash: corrupt the tail bytes of the on-disk file
    # directly (never calling client1.close(), so flags.DIRTY -- set on
    # open -- is never cleared), then abandon client1 without cleanup.
    with open(file_path, "r+b") as f:
        f.seek(data_end - 2)
        f.write(b"\xff\xff")

    client2 = custom_bson.MongoClient(str(data_dir))
    docs = list(client2.testdb.testcoll.find({}))
    assert len(docs) == 1
    assert docs[0]["a"] == 1
    client2.close()


# ---------------------------------------------------------------------------
# insert_many
# ---------------------------------------------------------------------------


def test_insert_many_ordered(collection):
    result = collection.insert_many([{"a": 1}, {"a": 2}, {"a": 3}])
    assert len(result.inserted_ids) == 3
    assert len(list(collection.find({}))) == 3


def test_insert_many_ordered_stops_at_first_error(collection):
    collection.create_index("a", unique=True)
    collection.insert_one({"a": 1})
    with pytest.raises(custom_bson.DuplicateKeyError):
        collection.insert_many([{"a": 2}, {"a": 1}, {"a": 3}], ordered=True)
    # first doc inserted before the failure stays inserted; the one
    # after the failure is never attempted
    assert collection.find_one({"a": 2}) is not None
    assert collection.find_one({"a": 3}) is None


def test_insert_many_unordered_continues_past_errors(collection):
    collection.create_index("a", unique=True)
    collection.insert_one({"a": 1})
    with pytest.raises(custom_bson.DuplicateKeyError):
        collection.insert_many([{"a": 2}, {"a": 1}, {"a": 3}], ordered=False)
    assert collection.find_one({"a": 2}) is not None
    assert collection.find_one({"a": 3}) is not None


# ---------------------------------------------------------------------------
# count_documents / estimated_document_count / distinct
# ---------------------------------------------------------------------------


def test_count_documents(collection):
    collection.insert_many([{"tag": "x"}, {"tag": "x"}, {"tag": "y"}])
    assert collection.count_documents({}) == 3
    assert collection.count_documents({"tag": "x"}) == 2


def test_count_documents_excludes_deleted(collection):
    collection.insert_many([{"tag": "x"}, {"tag": "x"}])
    collection.delete_one({"tag": "x"})
    assert collection.count_documents({}) == 1


def test_estimated_document_count(collection):
    collection.insert_many([{"a": 1}, {"a": 2}, {"a": 3}])
    assert collection.estimated_document_count() == 3
    collection.delete_one({"a": 1})
    assert collection.estimated_document_count() == 2


def test_distinct_simple(collection):
    collection.insert_many([{"tag": "x"}, {"tag": "y"}, {"tag": "x"}, {"tag": "z"}])
    assert sorted(collection.distinct("tag")) == ["x", "y", "z"]


def test_distinct_with_filter(collection):
    collection.insert_many([{"tag": "x", "n": 1}, {"tag": "y", "n": 2}, {"tag": "x", "n": 3}])
    assert sorted(collection.distinct("tag", {"n": {"$gte": 2}})) == ["x", "y"]


def test_distinct_array_field_returns_elements(collection):
    collection.insert_one({"tags": ["a", "b"]})
    collection.insert_one({"tags": ["b", "c"]})
    assert sorted(collection.distinct("tags")) == ["a", "b", "c"]


def test_distinct_bool_and_int_are_not_conflated(collection):
    collection.insert_many([{"v": True}, {"v": 1}, {"v": False}, {"v": 0}])
    values = collection.distinct("v")
    assert True in values and 1 in values
    assert len([v for v in values if v is True or v == 1]) == 2


# ---------------------------------------------------------------------------
# create_index / drop_index / list_indexes / index-accelerated queries
# ---------------------------------------------------------------------------


def test_create_index_returns_generated_name(collection):
    name = collection.create_index("age")
    assert name == "age_1"


def test_create_index_descending_name(collection):
    name = collection.create_index({"age": -1})
    assert name == "age_-1"


def test_create_index_is_idempotent_for_identical_spec(collection):
    name1 = collection.create_index("age")
    name2 = collection.create_index("age")
    assert name1 == name2


def test_create_index_conflicting_spec_raises(collection):
    collection.create_index("age", unique=False)
    with pytest.raises(InvalidDocument):
        collection.create_index("age", unique=True)


def test_create_index_on_already_populated_collection(collection):
    for i in range(50):
        collection.insert_one({"n": i})
    collection.create_index("n")
    assert [d["n"] for d in collection.find({"n": 25})] == [25]


def test_create_index_on_string_field_raises(collection):
    collection.insert_one({"s": "hello"})
    with pytest.raises(custom_bson.BSONNotImplementedError):
        collection.create_index("s")


def test_create_index_on_string_field_leaves_no_partial_file(collection):
    import os

    collection.insert_one({"s": "hello"})
    with pytest.raises(custom_bson.BSONNotImplementedError):
        collection.create_index("s")
    db_dir = os.path.dirname(collection._file_path)
    leftover = [f for f in os.listdir(db_dir) if f.endswith(".bidx")]
    assert leftover == []


def test_create_index_compound_keys_rejected(collection):
    with pytest.raises(custom_bson.BSONNotImplementedError):
        collection.create_index({"a": 1, "b": 1})


def test_list_indexes_empty_by_default(collection):
    assert collection.list_indexes() == []


def test_list_indexes_reports_created_indexes(collection):
    collection.create_index("age")
    collection.create_index("name", unique=True)
    indexes = {i["name"]: i for i in collection.list_indexes()}
    assert set(indexes) == {"age_1", "name_1"}
    assert indexes["age_1"]["unique"] is False
    assert indexes["name_1"]["unique"] is True
    assert indexes["age_1"]["key"] == {"age": 1}


def test_drop_index_removes_it(collection):
    name = collection.create_index("age")
    collection.drop_index(name)
    assert collection.list_indexes() == []


def test_drop_index_nonexistent_raises(collection):
    with pytest.raises(InvalidDocument):
        collection.drop_index("nope_1")


def test_unique_index_rejects_duplicate_and_leaves_no_orphan(collection):
    collection.create_index("uid", unique=True)
    collection.insert_one({"uid": 1})
    with pytest.raises(custom_bson.DuplicateKeyError):
        collection.insert_one({"uid": 1})
    assert len(list(collection.find({"uid": 1}))) == 1


def test_unique_index_allows_reinsert_after_delete(collection):
    collection.create_index("uid", unique=True)
    collection.insert_one({"uid": 1})
    collection.delete_one({"uid": 1})
    collection.insert_one({"uid": 1})  # must not raise
    assert len(list(collection.find({"uid": 1}))) == 1


def test_indexed_equality_query_matches_full_scan(collection):
    for i in range(300):
        collection.insert_one({"bucket": i % 17})
    collection.create_index("bucket")
    for target in (0, 5, 16):
        indexed = sorted(d["bucket"] for d in collection.find({"bucket": target}))
        full_scan = sorted(d["bucket"] for d in collection.find({}) if d["bucket"] == target)
        assert indexed == full_scan
        assert all(v == target for v in indexed)


def test_index_stays_correct_after_update_moves_value(collection):
    collection.create_index("age")
    r = collection.insert_one({"age": 1})
    collection.update_one({"_id": r.inserted_id}, {"$set": {"age": 2}})
    assert collection.find_one({"age": 1}) is None
    doc = collection.find_one({"age": 2})
    assert doc is not None and doc["_id"] == r.inserted_id


def test_index_stays_correct_after_delete(collection):
    collection.create_index("age")
    r1 = collection.insert_one({"age": 5})
    collection.insert_one({"age": 5})
    collection.delete_one({"_id": r1.inserted_id})
    remaining = list(collection.find({"age": 5}))
    assert len(remaining) == 1
    assert remaining[0]["_id"] != r1.inserted_id
