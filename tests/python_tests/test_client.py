"""Tests for MongoClient: a local facade over a data directory, not a
network client -- construction touches disk (creates the directory)."""

from __future__ import annotations

import os

import custom_bson
from custom_bson.database import Database


def test_construction_creates_data_directory(tmp_path):
    data_dir = tmp_path / "data"
    assert not data_dir.exists()
    client = custom_bson.MongoClient(str(data_dir))
    assert data_dir.is_dir()
    assert client.path == str(data_dir)
    client.close()


def test_attribute_access_returns_database(tmp_path):
    client = custom_bson.MongoClient(str(tmp_path / "data"))
    assert isinstance(client.mydb, Database)
    client.close()


def test_item_access_returns_database(tmp_path):
    client = custom_bson.MongoClient(str(tmp_path / "data"))
    assert isinstance(client["mydb"], Database)
    client.close()


def test_close_is_safe_with_no_open_collections(tmp_path):
    client = custom_bson.MongoClient(str(tmp_path / "data"))
    client.close()  # no collections ever opened -- must not raise


def test_close_flushes_and_closes_open_collections(tmp_path):
    client = custom_bson.MongoClient(str(tmp_path / "data"))
    client.mydb.mycoll.insert_one({"a": 1})
    client.close()
    # underlying handle is gone from the cache
    assert client._handles == {}


def test_data_persists_across_client_instances(tmp_path):
    data_dir = str(tmp_path / "data")
    client1 = custom_bson.MongoClient(data_dir)
    client1.mydb.mycoll.insert_one({"a": 1})
    client1.close()

    client2 = custom_bson.MongoClient(data_dir)
    assert client2.mydb.mycoll.find_one({"a": 1}) is not None
    client2.close()


def test_multiple_databases_get_separate_directories(tmp_path):
    data_dir = tmp_path / "data"
    client = custom_bson.MongoClient(str(data_dir))
    client.db_one.coll.insert_one({"v": 1})
    client.db_two.coll.insert_one({"v": 2})
    assert (data_dir / "db_one").is_dir()
    assert (data_dir / "db_two").is_dir()
    assert os.path.exists(data_dir / "db_one" / "coll.cbd")
    assert os.path.exists(data_dir / "db_two" / "coll.cbd")
    client.close()
