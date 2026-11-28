# Fuzzing the read path

`open_database_fuzz.cpp` feeds arbitrary bytes to `Database::open(bytes)` and then enumerates a
bounded prefix of each discovered table's rows. It is a libFuzzer target and requires Clang with
libFuzzer and sanitizer support.

Do not commit generated corpora or crash files. Keep seeds and findings in a gitignored directory
outside the source tree.

```sh
CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DREA_JACKCESS_BUILD_FUZZERS=ON
cmake --build build-fuzz --target rea_jackcess_fuzz_open

mkdir -p fuzz-work/corpus
./build-fuzz/fuzz/rea_jackcess_fuzz_open fuzz-work/corpus -max_total_time=300
```

Useful optional input directories:

- `build/fixtures/` for generated ACE/Jet4 fixtures;
- a directory containing the publicly licensed samples described in
  `test_data_only_read/README.md`, if they are available locally.

Reproduce a finding with:

```sh
./build-fuzz/fuzz/rea_jackcess_fuzz_open fuzz-work/crash-<id>
```

Crashes and timeouts should become deterministic regression tests under `tests/` before they are
fixed.
