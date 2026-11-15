# Command-line interface

`am` operates on standalone asset ZIP files and library directories. Editing an
existing asset through the CLI is planned for a later milestone. All text and JSON output is UTF-8.

Run `am --help` or `am asset <command> --help` for command help.

## Commands

```text
am asset create <files...> -o <out.zip> [--title <text>] [--json] [--verbose]
am asset inspect <zip> [--json] [--verbose]
am asset validate <zip> [--json] [--verbose]
am asset verify <zip> [--level quick|selective|full] [--file <FileID>]... [--json] [--verbose]
am asset extract <zip> -d <dir> [--file <FileID>]... [--manifest] [--json] [--verbose]
am library init <dir> [--name <text>] [--json] [--verbose]
am library info <dir> [--json] [--verbose]
```

### Library initialization and status

`am library init "My library" --name "Models"` initializes a new or empty directory.
Its parent must exist. It creates `assets/`, `trash/`, the `.am/` working directories,
and three SQLite databases, then publishes `am-library.toml` last. Initialization
does not retain a write-owner lock. Reinitializing an existing library or any
nonempty directory returns exit code 4 (`am.library.not_empty`).

An interrupted initialization may leave a nonempty directory without
`am-library.toml`. Inspect and remove that incomplete directory before retrying;
initialization does not repair or overwrite it.

`am library info "My library" --json` opens the library read-only. It reports
`library_id`, `name`, `created_at`, `schema_version`, `mode`, `capabilities`,
`write_unavailable_reason`, `owner` (`free` or `locked`), and `databases` with
`index`, `userdata`, and `state`. Each database has `state` and `user_version`
(null when unknown). States are `ok`, `missing`, `incompatible`, `newer`, or
`unavailable`. An open database also reports `read_mode` (`wal` or `immutable`);
an unavailable database reports `reason` (`wal_pending` or `open_failed`). The
index reports `rebuild_required` when its schema is understood. Locked-owner
diagnostics appear in `owner_detail` and do not establish ownership themselves.

Reading may create SQLite service files `-wal`/`-shm` beside existing databases.
Library data and main database files remain unchanged; missing databases are not
created. When the working directory is not writable, an existing database without
pending WAL pages is opened as immutable; a nonempty WAL instead produces
`unavailable`/`wal_pending` so uncheckpointed data cannot be silently omitted.
`info` returns 0 when the library configuration is understood, even if a database
is missing or unavailable. A directory without configuration returns 4; invalid
configuration returns 1. Unknown TOML keys are accepted without rewriting them.

### Create

```text
am asset create "parts/arm.stl" "images/front.png" -o "model.zip" --title "Example model"
```

The output must not already exist, and its parent directory must exist. There is
no overwrite option. Inputs must be files; directory traversal and recursive
import are not supported. Basenames are mapped to safe container paths. Colliding
names, including case and Unicode normalization collisions, are rejected before
file content is read. All files belong to one input and have `origin_kind=user_added`.

Creation uses a staging library named `.am-create-<uuid>` beside the output. The
repository commits the ZIP there, then the application publishes it without
replacing an existing destination. The published ZIP is fully verified. A failed
post-publication verification removes that new output. Staging is cleaned up on
normal success and handled failures.

Writing is currently enabled only for a fixed local NTFS volume on Windows.
Removable NTFS, RAM disks, network volumes, other filesystems, and non-Windows
systems return a read-only-storage error. Reading, verification, and extraction
do not require a writable asset library.

If the process is terminated, `.am-create-*` may remain beside the output. Automatic
cleanup of interrupted staging directories is not provided. A ZIP already
published before termination may also remain; verify it before use.

Successful output includes the asset ID, revision, file count, and output path.

### Inspect

Inspect validates V1/V2 and displays the format version and support state, asset
identity and revision, timestamps, title, counts, and current-file records. It does
not read payload streams or perform V3. A newer minor format is read in compatibility
mode and returns exit code 3; V2 is reported as `not_applicable_newer_minor` and V3
as `not_performed`.

### Validate and verify

`validate` performs V1–V3 without reading payload streams. `verify` uses the same
container verification implementation as the asset repository and additionally
reports V4:

- `full` (default) reads every registered stream, checking size, CRC, and SHA-256.
  Unregistered opaque streams can only receive CRC/size verification, so the result
  is partial.
- `quick` checks the manifest and structure without reading payload streams.
- `selective` checks the requested current FileIDs. Supply at least one `--file`;
  repeat the option to select more files.

Unverified streams and unverifiable semantic findings produce a partial result.
Warnings alone do not make a check fail. A validation-only command can pass even
when a payload is damaged; use full verification to check the bytes.

### Extract

The destination must be new or empty. Unknown FileIDs are rejected before writing.
Without `--file`, all current files are extracted under their `storage_path`.
Historical files, extension attachments, and opaque ZIP entries are not extracted.
`--manifest` also writes the original `manifest.json` bytes.

V1–V3 run before extraction. Failed validation prevents any extraction. Each selected
stream is written to a temporary file beside its destination, then checked against
the manifest size and SHA-256 before publication without replacement. A failed file
is removed from the temporary area; successfully extracted files remain. The result
lists extracted and failed FileIDs. Partial semantic validation remains partial
even if every selected file is extracted successfully.

## Output and errors

Standard output contains results. Errors and warnings go to standard error.
`--verbose` includes technical details and cause chains; normal error text uses
the embedded English message catalog. V3 findings use this text form:

```text
<severity> <rule> <JSON Pointer>: <message>
```

`--json` produces exactly one JSON object on standard output. Every object has a
`command` member (`create`, `inspect`, `validate`, `verify`, or `extract`). Validation
and verification preserve the repository verification report fields, including
`v3_findings`, `v3_complete`, and `v3_deferred_rules`. Extraction includes its
pre-extraction `verification` report and `extracted`/`failed` lists.

An operation error has this shape:

```json
{"command":"create","error":{"code":"am.conflict","message_id":"am.app.output_exists","detail":""}}
```

Technical `detail` can contain absolute paths. Create and extract also report their
result path directly. Check reports with failed levels are still verification
reports, rather than operation-error objects. Argument errors use CLI11 diagnostics
and exit code 2.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success; all performed checks passed. |
| 1 | A check failed (including invalid or unsupported ZIP structure at V1), an extracted hash/size did not match, or post-create verification failed. |
| 2 | Usage error: unknown command, invalid arguments, or selective verification without FileIDs. |
| 3 | Partial check: no failed level, but incomplete stream verification, unverifiable semantics, or a newer minor format. |
| 4 | The operation could not be completed: missing or unreadable input, I/O or resource failure, lock, insufficient space, existing output, or invalid extraction destination. |

## Development commands

The existing diagnostics remain available in the `Development` help group:

```text
am version
am uuid [--v4] [count]
am probe <file>...
am volumes <dir>
am zip-path <root> <source-path>
am fix-name <text>
am exclude <entry-path> [file]
```

These retain their diagnostic output. `uuid` generates UUIDv7 by default and UUIDv4
with `--v4`. They are not asset-editing commands.
