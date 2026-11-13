# Changelog

All notable user-visible changes are recorded here.

## [Unreleased]

### Added

- `am asset create` for new standalone asset ZIPs, with optional titles and safe
  publication without overwriting an existing file.
- `am asset inspect`, `validate`, and `verify` for manifest summaries and V1–V4
  verification, including selective FileID checks.
- `am asset extract` for verified extraction of current files and optional original
  manifest bytes into a new or empty directory.
- UTF-8 JSON output, stable exit codes 0–4, and an embedded English message catalog.
- Command help and the [CLI guide](docs/user/cli.md).

### Changed

- Manifest V3 checks physical defaults and claim units, required fields of base
  history changes, and excluded input directories according to ADR-0039.
- Command parsing uses CLI11, including UTF-8 Windows arguments. Existing
  development commands remain available in a separate help group.
- ZIP-path findings now identify the offending entry in technical details.
