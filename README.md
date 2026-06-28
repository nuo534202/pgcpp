# pgcpp

A C++20 reimplementation of the PostgreSQL — a readable path into database internals. It keeps PostgreSQL's original architecture intact and moves the code to modern C++. The goal isn't to replace PostgreSQL but to make its internals approachable — every module is ported from the C source behind a unit-test gate, and a ClickBench suite checks the whole system against stock PostgreSQL.

## Design

PostgreSQL's core mechanisms are kept as they are, because they define how the system behaves:

- Memory contexts with `palloc` / `pfree` for allocation
- `ereport` / `longjmp` for error recovery
- The `Node` hierarchy for parse and plan trees
- The `fork`-based process model

Modern C++ comes in where it makes the code clearer or safer, not as a rewrite for its own sake:

- `std::string` / `std::vector` back the legacy `List` / `StringInfo` interfaces
- C++ inheritance replaces manual `NodeTag` dispatch
- RAII guards manage context switches and resource lifetimes

A few features are avoided on purpose, since they clash with PostgreSQL's error and process models: C++ exceptions, `std::thread`, and C++ Modules.

Two checks gate every change: all 43 ClickBench queries must match stock PostgreSQL's output on the same dataset, and ASan, TSan, and UBSan must run clean.

## Tech Stack

| | |
| --- | --- |
| Language | C++20 |
| Compiler | GCC 11.4.0 |
| OS | Linux (Ubuntu 22.04) |
| Build | CMake 3.22.1, Make 4.3 |
| Dependencies | Google Test 1.17.0 (via CMake `FetchContent`) |
| Style | Google C++ Style Guide, 4-space indent |
| Sanitizers | ASan / TSan / UBSan |

## Build

Requires GCC 11.4.0+, CMake 3.22.1+, and Make 4.3+ (CMake enforces the compiler version at configure time). All commands run from the project root.

```bash
cmake -S . -B build
cmake --build build
```

Other configurations:

```bash
# Release
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release

# Sanitizers (ASan and TSan are mutually exclusive; UBSan is standalone)
cmake -S . -B build-asan  -DENABLE_ASAN=ON
cmake -S . -B build-tsan  -DENABLE_TSAN=ON
cmake -S . -B build-ubsan -DENABLE_UBSAN=ON
```

## Test

```bash
ctest --test-dir build                     # run everything
ctest --test-dir build --output-on-failure # show output on failure
ctest --test-dir build -R memory           # filter by name
ctest --test-dir build -j"$(nproc)"        # run in parallel
```

A sanitizer run passes only when its log is empty:

```bash
cmake --build build-asan
ctest --test-dir build-asan
```

## Code Quality

```bash
cmake --build build --target format        # apply clang-format
cmake --build build --target format-check  # verify formatting (CI)
cmake --build build --target tidy          # clang-tidy static checks
```

## Command-Line Tools

Both live in `tools/` and link against `mytoydb_core`:

| Binary | PostgreSQL equivalent | Purpose |
| --- | --- | --- |
| `mytoydb_initdb` | `initdb` | Initialize a new database cluster |
| `mytoydb_psql` | `psql` | Interactive query client |

## Project Layout

| Path | Purpose |
| --- | --- |
| `include/mytoydb/<module>/` | Public headers, per module |
| `src/<module>/` | Implementation, per module |
| `test/unit/<module>/` | Unit tests, per module |
| `test/benchmark/clickbench/` | ClickBench correctness suite (data + 43 queries) |
| `cmake/` | Build scripts: sanitizers, FetchContent, compiler warnings, code style |

## License

Released under the [MIT License](LICENSE).