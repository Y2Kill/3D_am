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
am library rebuild <dir> [--json] [--verbose]
am library sync <dir> [--full] [--json] [--verbose]
am library verify <dir> [--full] [--no-trash] [--json] [--verbose]
am library log <dir> [--asset <id>] [--job <id>] [--after <seq>] [--limit <n>] [--json] [--verbose]
am library list <dir> [--json] [--trash] [--verbose]
am library copy <dir> <asset-id> [--json] [--verbose]
am library trash <dir> <asset-id>... [--json] [--verbose]
am library restore <dir> <asset-id>... [--json] [--verbose]
am library purge <dir> (<asset-id>... | --all) --yes [--json] [--verbose]
am library add-ready <dir> <zip>... [--json] [--verbose]
am library add-ready <dir> <zip> --replace <current-manifest-sha256> [--json] [--verbose]
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

### Library index

`am library rebuild <dir> --json` takes the library write-owner lock and rebuilds
the index in one transaction from `assets/*/*.zip` and `trash/*.zip`. It does not
change asset ZIPs or user data. Existing readers see the previous index until
commit. The result contains `indexed`, `skipped` (path, reason, message ID),
`ignored`, and `duration_ms`. Unreadable ZIPs, unsupported formats, mismatched
IDs, and duplicate IDs are reported without failing the whole rebuild. Files
with other names are ignored. These files are never removed. A successful
rebuild returns 0 even when files were skipped; lock, read-only, I/O, and
transaction failures return 4.

Manifests that pass V1 and V2 are indexed. A newer minor format that passes V1
and reports V2 as `not_applicable_newer_minor` is indexed with `health: warn`
and detail `am.catalog.newer_minor`, provided its required summary fields have
the expected types. If V3 fails, the index retains the
asset summary, original manifest, and title-search entry with `health: corrupt`.
It omits child rows and filename search entries, and leaves `primary_preview`
null, because references and identifiers may be inconsistent. Newer minor
summaries use the same restrictions.

`am library sync <dir> --json` checks the same candidate paths in one transaction.
It reads added ZIPs and ZIPs whose relative path, size, or modification time has
changed. Unchanged ZIPs are not opened. Moving a ZIP between assets and trash
updates its indexed path. Duplicate IDs use the first relative path in sorted
order (`assets/` before `trash/`); other copies are reported as skipped.

Missing ZIPs retain their summary, child rows, and search entries with
`health: missing`. A ZIP that becomes unreadable retains its last indexed data
with `health: corrupt` or `unsupported` and the new size and modification time.
Restored missing ZIPs are always read again. If either `assets/` or `trash/`
cannot be enumerated, sync fails without marking any asset missing.

Use `--full` to read every candidate. This detects changes that preserve both
size and modification time. Sync refuses with exit code 4 when
`rebuild_required` is set; run `rebuild` first. Sync never clears that flag.
The result contains numeric `added`, `updated`, `unchanged`, newly marked
`missing` asset IDs, `skipped` (path, reason, message ID), `ignored`,
`duration_ms`, and boolean `full`. A committed sync returns 0 even with skipped
or missing files; text output warns about them. Lock, read-only, I/O, and
transaction failures return 4.

`am library list <dir> --json` reads the current index and returns `assets` plus
`rebuild_required`. Rows contain `asset_id`, `revision`, `title`, `health`,
`file_count`, `content_bytes`, and `in_trash`. Byte counts are decimal strings;
an unknown or overflowing total is null. `--trash` includes trash rows. A stale
index produces a warning and still returns 0. Use `sync` for disk changes or
`rebuild` when required. Automatic filesystem watching is planned separately. `info` also reports
`indexed_assets` (null if the index is unavailable).

### Recovery and library verification

Opening a library for writing first recovers pending operations and refreshes
the affected index rows. After an unclosed writer session, it also runs a
stat-based sync. An unavailable index is reported for rebuilding; ZIP recovery
still runs. A journal failure prevents opening for writing. Individual blocked
or unresolved operations produce warnings with their operation IDs and paths.
Read-only opening reports pending operations without recovering them.

`am library verify <dir>` checks the ZIP candidates on disk, including trash,
independently of index contents. The default quick check runs V1–V3 without
reading payload streams. `--full` also checks stream size, CRC, and SHA-256;
`--no-trash` excludes trash. ZIP bytes remain unchanged.

JSON contains `level`, `total`, `checked`, `ok`, `warn`, `corrupt`, `unsupported`,
`unreadable`, `cancelled`, `duration_ms`, `index_updated`, `items`, `skipped`,
`ignored`, and `recovery`. Items include the asset ID, relative path, trash and
index flags, status, message ID, and an optional JSON pointer or failed stream.
Only non-OK items appear by default; `--verbose` includes every checked asset.
Recovery reports recovered operations, reindexed IDs, the previous session
state, and optional startup sync. The service opening report also exposes the
read-only pending count.

With a usable writable index, verification records corrupt or unreadable indexed
assets as corrupt in one final transaction. It restores previously corrupt rows
after successful verification. It does not add unindexed ZIPs or change rows for
missing ZIPs. An unchanged ZIP retains its corruption mark during stat-based sync.
On Linux or storage without supported production writes, verification retries
read-only and reports `index_updated: false`; verification still runs.

Exit codes are 0 for OK/warnings, 1 for corrupt or unreadable assets, 3 for
unsupported assets or cancellation, and 4 for opening or operation errors.
The service API supports progress and cancellation between assets, committing
index results for the assets already checked. The CLI has no cancellation flag.

### Library operation log

`am library log <dir>` reads the persistent operation log in read-only mode,
including on Linux. It reports ready imports, independent copies, trash/restore/
purge, startup recovery, and library verification. Asset `create`/`change` history
stays inside the ZIP; `sync` and `rebuild` return reports without log entries.
A failed log append produces a warning and leaves the completed operation intact.

Entries are ordered by increasing `seq`. `--asset` and `--job` filter entries;
`--after` selects entries with a larger sequence number. `--limit` accepts 1–1000
and defaults to 100. JSON contains `entries` and `next_after` (the last sequence
number, or null for an empty page). Each entry contains `seq`, UTC `at`, `level`,
`code`, `job_id`, `asset_id`, technical `message`, and parsed `detail`. Exact JSON
numbers in details are retained. Text output uses event codes for translated
messages; `--verbose` adds technical diagnostics. Successful reads return 0,
including empty results; invalid arguments return 2.

Job storage now persists states, progress, decisions, and queue positions.
Opening for writing recovers interrupted running jobs after ZIP recovery and
reindexing; read-only opening reports them without changes. The attempt counter
increases at each actual start, rather than during recovery. Retry policy uses
an adjustable `max_attempts` (default 3, a hypothesis for measurement). There is
no executor or jobs CLI yet. Log rotation and diagnostic export are planned
separately; there is no automatic history cleanup.

### Independent asset copy

`am library copy <dir> <asset-id>` creates a separate asset in the same library.
It reads one snapshot of the active source, assigns a new AssetID and new local
instance IDs, and remaps their references and `content/<InputID>/` paths.
Payload bytes and their SHA-256 hashes remain unchanged; compressed ZIP streams
are transferred without recompression. The source ZIP remains unchanged.

The copy starts at revision 1 with one `asset_created` history entry recording
`creation_kind: independent_copy` and the source AssetID and revision. Previous
history, annotations, collection membership, and dismissed suggestions are not
copied. New asset timestamps reflect the copy operation. Contract identifiers
such as FieldID and TypeID remain unchanged.

The current implementation refuses extensions, plugin field values, historical
files, references to deleted files, and unregistered ZIP entries. It also refuses
when a source instance ID remains embedded in other JSON data after remapping.
The error detail identifies the first remaining JSON reference. Newer minor
formats are read-only for this operation. Restore a trashed source before copying.
Ordinary ZIP copying preserves IDs and remains available.

JSON contains `source: {asset_id, revision}`, the new `asset_id`, `revision: 1`,
`files` (new FileIDs and storage paths), and `error: null` on success. On refusal,
unavailable result fields are null and `error` contains the stable message ID.
`--verbose` adds the successful copy's verification report. Exit code 0 means
the copy was committed; 4 means refusal or failed verification; 2 means invalid
arguments. An index-update warning leaves the committed ZIP in place and marks
the index for rebuilding.

### Library trash

`trash` moves an asset from `assets/` to `trash/<asset-id>.zip`; `restore` moves it
back. These operations keep ZIP bytes, revision, and asset history unchanged.
They also work on damaged ZIPs. An occupied destination is a conflict: neither
file is replaced. An asset in trash must be restored before its contents can be
edited. `list` hides trash by default; `list --trash` includes it.

`purge` permanently deletes files from trash. It requires `--yes`; omission
returns exit code 2 with `am.cli.purge_confirmation_required`. `--all` selects
regular, correctly named UUIDv4 ZIPs in trash. Other files, directories, and
symbolic links are left in place. There is no automatic cleanup.
An absent trash directory is an empty selection: `purge --all --yes` returns 0.

Each requested ID is processed separately. JSON contains `results`, with
`asset_id`, `status` (`done` or `failed`), and `error` (null on success). Exit code
0 means all selected operations completed; 4 means at least one failed. A failed
ID does not stop processing the remaining IDs. Index-update warnings leave the
file operation committed and set `rebuild_required`; run `rebuild` to repair the
index. The operations use the per-asset lock and recovery journal.

`info` includes `trash: {assets, bytes}` from the current index. `bytes` is an
exact decimal string, or null with a warning if the sum exceeds uint64. `trash`
is null when the index requires rebuilding or is unavailable. User data and
collection membership are retained by all three operations, including purge.

### Import a ready asset

`add-ready` imports complete asset ZIPs into a library without changing their
bytes, ID, revision, history, or metadata. Only explicit file paths are accepted;
directories, recursive scans, and wildcard expansion are not supported. Sources
must be outside the destination library. Use `sync` for files already placed there.

The temporary copy receives full V1-V4 verification before publication. Every
registered stream must pass SHA-256 verification. A newer minor version can be
stored unchanged with a warning; unsupported major versions and damaged assets
are refused. Ordinary ZIPs and non-ZIP files are reported as `not_own`; use
`asset create` to create an asset from source material.

Sources are processed in order. A repeated manifest is skipped as `identical`,
with `zip_bytes_identical` showing whether the complete ZIP bytes also match.
Different revisions and different manifests at the same revision are conflicts;
a higher revision never replaces an asset automatically. A matching ID in trash
requires restore or purge first. An unreadable current asset also requires an
explicit trash/purge operation before it can be replaced.

To replace a readable conflict, pass its returned `current.manifest_sha256` to
`--replace` with exactly one source. The current manifest must still match that
hash. An explicitly selected older revision produces a warning. User data is
retained. Independent copies with new IDs are planned separately.

JSON contains `results` with `source`, `asset_id`, `classification`, `status`,
`incoming`, `current`, `zip_bytes_identical`, and `error`. Status is `imported`,
`identical`, `replaced`, `conflict`, or `failed`. Metadata includes revision,
manifest SHA-256, ZIP size as a decimal string, and minor version; an unreadable
current asset reports only ZIP size. Verification details appear with `--verbose`
and on verification failure. Exit code 0 means all sources were imported,
replaced, or identical; 4 means at least one conflict/failure; 2 means invalid
command arguments. Index warnings leave the ZIP registered and require a rebuild.

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
