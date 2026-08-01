# Development guide

This guide covers everything needed to build `bsondb` from source,
run its test suites, and find your way around the repository. For the
Python API itself, see [`docs/api_reference.md`](api_reference.md).

## Table of contents

- [Prerequisites](#prerequisites)
- [Setup](#setup)
  - [Windows](#windows)
  - [macOS](#macos)
  - [Linux](#linux)
- [Taskfile reference](#taskfile-reference)
- [Manual setup (without Task)](#manual-setup-without-task)
- [Folder structure](#folder-structure)
- [Running tests](#running-tests)
- [Troubleshooting](#troubleshooting)

## Prerequisites

`bsondb` ships two CPython extension modules
(`bsondb._bson_core`, `bsondb._storage_core`), so a C toolchain is
required in addition to Python -- there are no prebuilt wheels.

| Tool | Version | Required for | Notes |
| --- | --- | --- | --- |
| Python | 3.9+ | Everything | `python -m venv` must be available |
| A C11 compiler | -- | `pip install -e .` (the extensions) | MSVC on Windows, GCC/Clang on Linux/macOS |
| [Task](https://taskfile.dev) | 3.x | Optional, but recommended | Runs the `Taskfile.yml` targets below |
| CMake | 3.15+ | Optional | Only for the standalone C engine + C smoke tests |
| `pytest` | 7+ | Running the Python test suite | Installed automatically via `pip install -e ".[dev]"` |

The package itself has **no runtime dependencies** beyond the Python
standard library and its own native extensions.

## Setup

Each OS section shows the fastest path (using [Task](https://taskfile.dev))
end to end. If you don't want to install Task, skip to
[Manual setup](#manual-setup-without-task).

### Windows

1. Install [Python 3.9+](https://www.python.org/downloads/) and add it to `PATH`.
2. Install the MSVC build tools: [Visual Studio Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/)
   with the "Desktop development with C++" workload (provides `cl.exe`).
3. Install [Task](https://taskfile.dev/installation/) (e.g. `winget install Task.Task`
   or `choco install go-task`).
4. From a shell that can see `cl.exe` (a "Developer PowerShell for VS"
   or after running `vcvarsall.bat`), run:

   ```powershell
   task install
   task test
   ```

### macOS

1. Install Xcode Command Line Tools for a C compiler: `xcode-select --install`.
2. Install Python 3.9+ (Homebrew: `brew install python`).
3. Install Task: `brew install go-task/tap/go-task`.
4. Run:

   ```bash
   task install
   task test
   ```

### Linux

1. Install a C compiler and Python headers, e.g. on Debian/Ubuntu:

   ```bash
   sudo apt install build-essential python3 python3-venv python3-dev
   ```
2. Install Task (see [taskfile.dev/installation](https://taskfile.dev/installation/)), e.g.:

   ```bash
   sh -c "$(curl --location https://taskfile.dev/install.sh)" -- -d -b ~/.local/bin
   ```
3. Run:

   ```bash
   task install
   task test
   ```

## Taskfile reference

All common workflows are defined in [`Taskfile.yml`](../Taskfile.yml)
and are cross-platform (Windows/Linux/macOS). Run `task --list` or
`task` with no arguments to see this list from the CLI.

| Task | Description |
| --- | --- |
| `task` / `task default` | List all available tasks |
| `task venv` | Create the `.venv` virtual environment if it doesn't exist |
| `task install` | Create the venv and `pip install -e ".[dev]"` into it |
| `task test-py` | Run the Python test suite (`pytest tests/python_tests/ -v`) |
| `task build-c` | Configure and build the standalone C engine + C smoke tests via CMake |
| `task test-c` | Build (if needed) and run the C smoke tests via `ctest` |
| `task test` | Run both `test-py` and `test-c` |
| `task clean` | Remove build artifacts (`build/`, `.pytest_cache/`, `python/bsondb.egg-info/`) -- keeps `.venv` |
| `task clean-venv` | Remove the `.venv` virtual environment entirely |

## Manual setup (without Task)

```bash
# 1. Create and activate a virtual environment
python -m venv .venv
.venv\Scripts\activate      # Windows
source .venv/bin/activate   # Linux / macOS

# 2. Install in editable mode with dev dependencies
pip install -e ".[dev]"

# 3. Run the Python test suite
pytest tests/python_tests/ -v

# 4. (Optional) build and run the standalone C engine + smoke tests
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Folder structure

```
bsondb/
├── include/custom_bson/     Public C headers: BSON engine, mmap abstraction,
│                             storage/record format, B-Tree index
├── src/
│   ├── c_engine/             Storage/BSON engine implementation (pure C11,
│   │                         no Python dependency)
│   └── python_bindings/      CPython C-API bindings:
│                             _bson_core.c (encode/decode), _storage_core.c
│                             (storage/index)
├── python/bsondb/            Pure-Python package: ObjectId, exceptions,
│                             BsonDBClient / Database / Collection,
│                             query/index/cursor logic
├── tests/
│   ├── c_tests/               C smoke tests (assert()-based, run via ctest)
│   └── python_tests/          Python test suite (pytest)
├── docs/                     Wire protocol spec, API reference, this guide
├── CMakeLists.txt            Optional build graph for the standalone C engine
│                             + C smoke tests (not used by `pip install -e .`)
├── setup.py                  setuptools glue that builds the two native
│                             extension modules used by `pip install -e .`
├── pyproject.toml            PEP 517 project metadata
└── Taskfile.yml               Cross-platform dev task runner (see above)
```

A document flows through the system roughly as:

1. Python `dict` → `bsondb._bson_core.encode()` → BSON `bytes`
   (see [`docs/wire_protocol.md`](wire_protocol.md)).
2. BSON `bytes` → `bsondb._storage_core` appends a
   `status_byte + BSON document` record to the collection's mmap'd
   `.cbd` file (`include/custom_bson/storage.h`).
3. If the field is indexed, an entry is written to the field's
   `.bidx` B+Tree file (`include/custom_bson/btree.h`).
4. `python/bsondb/collection.py` and friends provide the
   PyMongo-like `Collection`/`Cursor`/query layer on top of the two
   extension modules.

## Running tests

```bash
# Python test suite only
task test-py
# or: pytest tests/python_tests/ -v

# C smoke tests only
task test-c
# or: cmake -S . -B build -DBUILD_TESTING=ON && cmake --build build
#     ctest --test-dir build --output-on-failure

# Everything
task test
```

## Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `error: Microsoft Visual C++ 14.0 or greater is required` (Windows) | Install the MSVC Build Tools "Desktop development with C++" workload, then re-run from a Developer shell |
| `pip install -e ".[dev]"` fails with a missing `Python.h` (Linux) | Install your distro's Python dev headers, e.g. `python3-dev` / `python3-devel` |
| `task: command not found` | Task isn't installed or isn't on `PATH` -- see the [install docs](https://taskfile.dev/installation/), or fall back to [manual setup](#manual-setup-without-task) |
| `cmake: command not found` | CMake is only needed for `task build-c`/`test-c` (the optional standalone C engine); the `pip install -e .` path doesn't need it |
| Stale native extension after editing `.c`/`.h` files | Re-run `pip install -e ".[dev]"` (or `task install`) to rebuild the extensions |
