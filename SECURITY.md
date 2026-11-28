# Security policy

## Supported versions

Security fixes are provided for the latest `0.1.x` release. Older pre-release snapshots and
untagged commits are not supported.

## Reporting a vulnerability

Do **not** open a public issue for a suspected vulnerability.

Use GitHub's private vulnerability reporting / Security Advisories for this repository. Include:

- the affected version or commit;
- a minimal reproducer or the exact bytes needed to reproduce;
- the platform, compiler and build options;
- whether the result is a crash, hang, excessive memory use, or incorrect data exposure.

If private reporting is unavailable, contact the repository maintainers through GitHub and ask for
a private channel before sending details.

## Scope

rea-jackcess parses untrusted database files. Reports are in scope when they involve:

- memory-safety defects (out-of-bounds read/write, use-after-free, integer overflow);
- denial of service through malformed files (unbounded allocation, pathological loops or very long
  processing time);
- unintended file modification by a read-only open;
- disclosure of data outside the requested database or linked source.

The following are known limitations, not vulnerabilities, unless they cause memory corruption or a
denial of service:

- encrypted ACE databases are rejected without a `PageCodec`;
- Jet3 text-key equality lookup is unsupported;
- complex-value and expression evaluation writes are unsupported;
- PGeo geometry is returned as raw bytes and is not decoded.

## Response

Maintainers will acknowledge a report, investigate it, and coordinate a fix and disclosure on a
best-effort basis. Please allow time for a fix before public disclosure.

## Hardening

The read path is covered by Linux x86_64/ARM64 tests, ASan/UBSan builds, and a libFuzzer target
(`fuzz/`). Reports with new fuzz findings or regression inputs are welcome under this policy.
