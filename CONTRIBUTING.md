# Contributing

Thanks for helping improve rea-jackcess.

## Scope

The current release line is a **read-only** MDB/ACCDB reader. Do not introduce new write features
unless they are part of an explicitly reviewed work package. Changes to the fixed Jackcess 5.2.0
baseline require a documented difference review and updated regression evidence.

## Before you start

- Read `README.md`, `src/ARCHITECTURE.md`, and the roadmap documents under `plan/Jackcess-C++重写/`.
- Check existing issues and pull requests before starting a large change.
- For a new public API, describe the corresponding Jackcess 5.2.0 behaviour and the evidence that
  will validate it.

## Build and test

Linux:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
(cd build && ctest --output-on-failure)
```

Windows development builds are documented in `README.md`; they are development evidence, not a
replacement for the Linux release CI.

Some tests need generated fixtures or locally held samples. When a sample is absent, the test is
registered only when the corresponding CMake cache path exists, or exits with code 77 so CTest
reports it as skipped. Do not weaken a test to hide a missing sample.

## Code and comments

- C++17, no compiler extensions.
- Public API lives in namespace `rea_jackcess`; internals in `rea_jackcess::detail`.
- Public and internal headers use English Doxygen comments: purpose, every parameter, return value,
  and thrown errors or standard exceptions. Struct/enum members document meaning, unit and encoding.
- Parse untrusted files defensively: check lengths, page/row references, integer overflow and loops;
  report context (file, page, table, row, column) in errors.
- Do not silently rewrite unknown or unsupported structures. Either reject explicitly or preserve
  raw bytes.

## Samples and fixtures

- Do not commit new binary databases (`.mdb`, `.accdb`).
- Generate fixtures with the generators under `tools/validation/` into the build directory; the
  default is `build/fixtures` (`REA_JACKCESS_FIXTURE_DIR`).
- The only tracked real sample is `test_data_only_read/test_mdb.mdb`. Do not replace it or add
  similar samples without maintainer approval and a licensing/privacy review.
- Keep downloaded validation corpora out of git; record source, license and SHA-256 in
  `test_data_only_read/README.md`.

## Tests and evidence

- Every behaviour change needs a regression test unless the change is documentation-only.
- Record which capabilities are verified, partially verified, or unverified. A synthetic test or a
  single sample does not establish general format compatibility.
- If you change a reported compatibility result, update the corresponding matrix, roadmap and release
  record under `plan/Jackcess-C++重写/`.

## Pull requests

- Keep changes focused and explain the user-visible effect.
- Include the commands you ran and their results.
- Run `git diff --check` and the relevant CTest subset before opening the PR.
- By submitting a contribution you agree that it is licensed under the Apache License 2.0, matching
  this repository.

## Security issues

Do not open a public issue for a suspected vulnerability. Follow `SECURITY.md`.
