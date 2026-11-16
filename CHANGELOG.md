# Changelog

All notable user-visible changes are recorded here.

## [Unreleased]

### Added

- Incremental `am library sync` with stat-based change detection, retained missing
  and unreadable asset summaries, atomic updates, and `--full` rereading.
- Transactional library index rebuilds with `am library rebuild`, read-only
  `am library list`, indexed-asset counts in `info`, and post-commit indexing.
- Exact byte-size totals and preserved custom-field JSON in the disposable index.
- Summary indexing of schema-valid, semantically corrupt assets without projecting
  inconsistent child records; unvalidated manifests are reported as skipped.
- `am library init` and read-only `am library info`, including JSON database
  versions, rebuild status, storage capabilities, and write-owner diagnostics.
- Library storage foundation with a lifetime write-owner lock, three versioned
  SQLite databases, migration backups, and disposable-index recreation.

- Asset format 1.0 is published: normative schema
  [`schemas/manifest-1.0.schema.json`](schemas/manifest-1.0.schema.json) and the
  golden ZIP corpus in [`tests/fixtures/golden/`](tests/fixtures/golden/README.md).
  Published 1.0 cases are immutable; future changes require a new format version.
- `am asset create` for new standalone asset ZIPs, with optional titles and safe
  publication without overwriting an existing file.
- `am asset inspect`, `validate`, and `verify` for manifest summaries and V1–V4
  verification, including selective FileID checks.
- `am asset extract` for verified extraction of current files and optional original
  manifest bytes into a new or empty directory.
- UTF-8 JSON output, stable exit codes 0–4, and an embedded English message catalog.
- Command help and the [CLI guide](docs/user/cli.md).

### Changed

- Newer minor asset formats are indexed as warning summaries without projecting
  unverified child records or filename search entries.
- Duplicate manifest IDs no longer fail V2 schema validation; V3 reports them,
  while rewriting documents with ambiguous opaque owners remains blocked.
- Asset checks reject ZIP entries marked as symbolic links or other non-regular files.
- Invalid or unsupported ZIP structure is reported as a failed V1 check with exit
  code 1; missing inputs and I/O failures remain operation errors with exit code 4.
- Assets with foreign ZIP extra fields or entry comments can now be changed;
  rewritten headers omit that metadata while preserving compressed payload bytes.
- Manifest V3 checks physical defaults and claim units, required fields of base
  history changes, and excluded input directories according to ADR-0039.
- Command parsing uses CLI11, including UTF-8 Windows arguments. Existing
  development commands remain available in a separate help group.
- ZIP-path findings now identify the offending entry in technical details.
