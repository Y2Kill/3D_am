# Changelog

All notable user-visible changes are recorded here.

## [Unreleased]

### Added

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
