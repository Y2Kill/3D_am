# Changelog

All notable user-visible changes are recorded here.

## [Unreleased]

### Fixed

- Missing library state databases are recreated with a persistent warning and
  historical timestamp. Recovery retains orphan files for one opening when the
  journal was recreated or may have lost its WAL. Corrupt state is not recreated.
- SQLite disk-full failures report insufficient space with recovery actions;
  a confirmed ZIP survives a failed index update.
- Library initialization reports disk-full configuration writes and removes
  the unconfirmed temporary configuration.
- A newly initialized library has a complete empty index and can synchronize
  without an initial rebuild. Recreated missing or damaged indexes still require one.
- Full export preserves assets from newer format 1.x minor versions byte for byte;
  selective export continues to require a supported manifest version.
- Explicit asset IDs cannot be reused while the original asset is in the trash.
  Retrying an import in this state asks for a decision and preserves its IDs.
- Format 1.0 validation accepts long original names and dotted file type IDs on
  Windows, using schema-equivalent linear checks for the affected patterns.
- Disabled logging avoids shared-pointer loads, and log quota cleanup tolerates files held by other processes.
- Import scanning distinguishes links from other Windows reparse tags, skips online-only file reads, and asks about conflicting service filenames and content signatures.
- Diagnostic exports report skipped cloud placeholders without reading them.
- Library opening avoids full database scans during clean sessions. Verification
  checks all three databases; unclosed writer sessions check integrity before recovery.
- FTS row deletion uses indexed ownership keys. Index schema version 2 is rebuilt
  from unchanged ZIPs, avoiding full FTS scans during writes and rebuilds.

### Added

- End-to-end CLI tests cover isolated library transfers, index reconstruction,
  independent copies, trash round trips and selective exports. A Windows bundle
  checks ZIP and library read portability on Linux.
- A generated import regression corpus covers all sixteen scenario IDs through
  the real CLI, with declarative expectations, source preservation checks and
  explicit filesystem restrictions on Linux.

- `am export` copies a retained asset revision byte for byte or exports selected
  current files as an ordinary ZIP. Portable names, publication without replacement,
  resumable export jobs and direct export from read-only libraries are supported.

- Import expands ZIP archives nested inside ZIP inputs (plan version 3). Stored
  nested archives are read in place; compressed ones are unpacked into the
  library's temporary area. Depth, temporary space and total unpacked bytes are
  limited, and the original nested archive is recorded but not stored.
- A 3MF inside a ZIP input is checked for its `3D/3dmodel.model` part: verified
  files are included with certainty, others ask whether to keep or expand them.
  Opening a library removes import temporary areas older than the orphan age.
- Ordinary ZIP inputs can be scanned into editable version 2 import plans and
  imported without temporary extraction, with original names, per-record outcomes,
  archive hashes, shared limits, and source freshness checks. Version 1 plans remain readable.
- Nested archives can be preserved as files or excluded; expansion remains deferred.
- Durable `am import run` for edited file/directory plans and ready assets, with
  source freshness checks, partial import, resumable jobs, and explicit deferred outcomes.

- Read-only `am import scan` with versioned profiles, editable import plans,
  suggested boundaries, exclusions and volume hints, separate ready assets,
  optional library matches, and safe cancellation and plan publication.
- Explicitly enabled JSON Lines process logs with rotation, job and library
  context, and local diagnostic ZIP export with per-export path tokens and
  identity redaction. Logging failures do not change command results.
- Cooperative background job execution, bounded resource profiles, safe pause and
  shutdown, decisions, parent jobs, and isolated failures. `am jobs` manages the
  queue; `am library verify --enqueue` schedules verification and `am media`
  reports the detected storage type.
- Deterministic synthetic library generation and process-based catalog scale measurements,
  with resumable public-API writes and checks for ZIP-free clean catalog startup.

- Persistent job storage with validated transitions, exact JSON, progress,
  decisions, queue ordering, and recovery after interrupted writer sessions.
- Library operation logging and read-only `am library log` with filtering and
  sequence-based pagination. Log failures warn without undoing completed operations.
- `am library verify` with quick and full checks, optional trash exclusion,
  per-asset reports, read-only fallback, and atomic corruption updates in the index.
- Automatic recovery when opening a library for writing, affected-asset indexing,
  and stat-based sync after an unclosed writer session.
- `am library copy` for independent assets with new instance IDs, remapped
  references, revision-one source history, and unchanged compressed payloads.
  Unsupported extension, plugin-value, historical, or opaque references are refused.
- `am library add-ready` for verified byte-preserving asset imports, explicit
  manifest-hash replacement, ID conflict reports, and unchanged newer-minor storage.
- Library `trash`, `restore`, and confirmed `purge` commands, with recovery
  journaling, per-asset results, preserved ZIP bytes on moves, and indexed trash size.
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

- `am library purge --all --yes` succeeds with an empty selection when the trash
  directory is absent.
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
