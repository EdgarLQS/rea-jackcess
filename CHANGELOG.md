# Changelog

All notable changes to rea-jackcess are documented here. The project follows semantic versioning
for public API changes; pre-1.0 releases may still change the API between minor versions.

## Unreleased

## 0.1.0 - 2026-11-28

First scoped read-only release.

- Jet3, Jet4 V2000/V2003, ACE V2007/V2010/V2016/V2019 reads for ordinary tables: schema, rows,
  values, indexes, relationships, property maps and saved-query metadata.
- Jet3 code pages 1252/819/28591/936/932/1251, including real 936/932 corpora.
- ACE complex columns: conceptual/flat-table links and typed read-only values for multi-value,
  attachment, version-history and unsupported values.
- ACE `EXT_DATE_TIME`, calculated-column result types and stored results (no formula evaluation).
- PGeo adapter: layer enumeration, OBJECTID, typed attributes and raw SHAPE bytes.
- CSV export/import for ordinary tables and complex foreign keys.
- Read-only `PageCodec` contract; encoded or encrypted databases are rejected without a codec.
- CMake install/export package (`find_package(rea_jackcess CONFIG REQUIRED)`) and pkg-config file.
- Linux x86_64/ARM64 CI, sanitizer builds, performance gate, and a libFuzzer target.

Known limitations are documented in `plan/Jackcess-C++重写/16-可读特性与已知差异.md`, including
AR-10 (real non-marked LONG complex foreign key), encrypted page decryption, Jet3 text-key equality
lookup, and the alternate DAO extended-date "second form".
