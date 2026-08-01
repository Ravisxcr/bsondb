"""setuptools glue for the jsondb native extensions.

Authoritative build path for `pip install -e .` (see pyproject.toml for
the PEP 517 metadata). Two extension modules are built:

  - jsondb._bson_core: BSON encode/decode only (bson_engine.c +
    _bson_core.c).
  - jsondb._storage_core: the collection data file storage engine
    (bson_engine.c + platform_mmap.c + storage.c + scanner.c +
    _storage_core.c).

bson_engine.c is compiled into both extensions' sources rather than
built once and linked -- no ODR issue since neither shares a .so, and
setuptools has no clean cross-platform way to link one extension
against another. CMakeLists.txt is a separate, optional build graph
for the standalone C engine and C smoke tests; it is not invoked by
this file.
"""
import sys

from setuptools import Extension, setup

if sys.platform == "win32":
    extra_compile_args = ["/std:c11", "/W4"]
else:
    extra_compile_args = ["-std=c11", "-Wall", "-Wextra"]

bson_core_extension = Extension(
    "jsondb._bson_core",
    sources=[
        "src/c_engine/bson_engine.c",
        "src/python_bindings/_bson_core.c",
    ],
    include_dirs=["include"],
    extra_compile_args=extra_compile_args,
)

storage_core_extension = Extension(
    "jsondb._storage_core",
    sources=[
        "src/c_engine/bson_engine.c",
        "src/c_engine/platform_mmap.c",
        "src/c_engine/storage.c",
        "src/c_engine/scanner.c",
        "src/c_engine/btree.c",
        "src/python_bindings/_storage_core.c",
    ],
    include_dirs=["include"],
    extra_compile_args=extra_compile_args,
)

setup(ext_modules=[bson_core_extension, storage_core_extension])
