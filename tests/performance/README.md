# Synthetic catalog scale measurements (K-112)

These tools collect data for the [measurement plan](../../docs/architecture/nfr_measurement_plan.md).
They set no performance thresholds. K-112 remains partial: search, filtering, GUI first results,
GUI memory and cold OS caches are not measured yet.

## Build and generate

The CLI test configuration builds `am_gen_library` alongside `am`. No new dependencies are needed.
Use a Release or RelWithDebInfo CLI build for published measurements. Keep generated libraries,
source scratch files and build caches under the project's `.cache/perf/`; never commit them.

```text
am_gen_library --out .cache/perf/example --count 100 --seed 1 --json
am_gen_library --out .cache/perf/example --count 100 --seed 1 --plan-only --json
```

Options: `--out DIR`, `--count N` (1..250000), `--seed S` (uint64, default 1),
`--trash-fraction F` (0..1, up to six decimal places, default 0.01),
`--content-scale K` (positive integer, default 1), `--plan-only`, `--json`,
`--assume-capabilities`, and the test interruption option `--stop-after K`.
The task's scale starts at 100; smaller counts support targeted local checks.

Each asset is written through public `Catalog::create`; trash uses `Catalog::trash`.
ZIP commits, validation and synchronization are enabled throughout.
The generator stores semantic parameters in `DIR.gen.json`, checks them before opening the library,
and refuses mismatches without modifying the library or descriptor. A nonempty unmarked directory
is refused. A matching rerun creates only missing assets and completes planned trash moves.
`DIR.sources/` contains reusable synthetic source files outside the library; it is not measured as
library disk usage. Resume requires the same count, seed, fraction and content scale.

On Linux/WSL use `--assume-capabilities` for synthetic generation. It injects the existing test
capabilities; output always records `assume_capabilities` explicitly. Published Windows results use
a verified fixed NTFS volume without this override. The production `am` CLI has no such override.

## Deterministic logical plan

Plan version 1 uses splitmix64 with unsigned 64-bit arithmetic and explicit rejection mapping
`threshold = (0-n) % n; accept x >= threshold; x % n`. Each zero-based asset index has its own
stream, initialized by `seed XOR ((index+1)*0xd1342543de82ef95)`. UUID bytes consume the low byte
of successive outputs, with RFC UUIDv4 version and variant bits set. Increasing count preserves
all earlier assets. The trash flag changes at each increment of `floor((index+1)*fraction)`.

The canonical plan is compact UTF-8 JSON, in the explicitly constructed insertion order in
`Asset::canonical()`, followed by LF for each asset. Keys: `asset_id`, `op_id`, `input_id`,
`metadata`, `trash`, `files`; each file has `path`, `file_id`, integer `size`, and lowercase
`sha256`. JSON whitespace and escaping follow the existing ordered JSON library.
The SHA-256 for seed 1 / count 100 / default fraction and scale is:

```text
ff6dad951f904b063f1388ad1fa39b3c0764bfb75ec26af3b4ddf6ab5f23f767
```

`--plan-only` performs no writes and additionally returns each asset's canonical SHA-256.
System timestamps, library UUID, physical ZIP bytes and the service `trash` operation ID in
`op_log` are excluded from logical determinism (owner decision V-3). Creation/history operation
IDs, input/file/asset IDs, metadata, paths, payloads and trash selection are deterministic.
Payloads have a unique little-endian `(seed,index,file_index)` prefix, followed by stream bytes.

File count uses a small-skew discrete distribution (1..50, median 4). File sizes default to
64..16384 bytes. Tags use a 500-word integer harmonic CDF (Zipf), categories use 100 paths,
and metadata includes Latin, Cyrillic, CJK, spaces and punctuation. Payload extensions describe
synthetic bytes, not valid model/image files. Previews and custom_values are omitted because
public repository operations for them are not yet available.

## Run measurements

```text
python -I tests/performance/run_scale.py --am BUILD/bin/am.exe --generator BUILD/bin/am_gen_library.exe --root .cache/perf/run-unique --storage nvme-ssd --build-type Release --output tests/performance/results/YYYY-MM-DD-windows-release-nvme.json
```

Use a fresh root for every published run. Counts default to 100, 1000, 10000, 50000, 100000,
250000; `--counts` selects an ascending subset. Mandatory first-run counts are 100/1000/10000.
For each larger volume, the generator rate at 10000 estimates its generation time; a value over
3 hours causes an explicit unmeasured entry, never an unapproved long generation.
`--repeat` defaults to 5 (total runs). `--seed` defaults to 1.
The storage label is supplied by the operator (`nvme-ssd`, `sata-ssd`, `hdd`, etc.); it is never
guessed. Filesystem, CPU, logical cores, physical RAM and OS version are read without collecting
hostname, username or serial numbers. GPU is null when not detected.

`--library DIR --generation-report FILE --counts N` measures an already generated library using
its generator JSON. This is useful for smoke checks or a resumed measurement; the descriptor must
match the report. Its generation timing describes the supplied generation invocation, not an
invented uninterrupted duration.

Operations run as separate processes: `version`, library `info`, `list --json`, unchanged stat
`sync`, quick `verify`, and finally `rebuild`. The first sample of each operation is recorded as
`first_after_generation`; this label does not claim a cold cache. Remaining samples provide
median, nearest-rank P95, minimum and maximum child peak memory. When the P95 rank selects the
maximum, `p95_method` is `maximum_small_sample`. With repeat 1 statistics is null.
Windows reads `PeakWorkingSetSize` with GetProcessMemoryInfo before closing the process handle;
Linux uses the specific child's wait4 ru_maxrss in bytes. Output is spooled to cache files so
large list output neither blocks pipes nor contaminates child memory with parent buffers.
A nonzero exit fails measurement and creates no sample. On Linux, sync/rebuild are explicitly
`not_measured` with reason `storage_read_only` (owner decision V-2); all remaining operations run.

## Result format: am.perf.scale.v1

Results under `results/YYYY-MM-DD-config.json` contain:

- `schema`, UTC `date`, `program` (version, source commit, build_type), `repeat`,
  `environment` (os/os_version/cpu/logical_cores/ram_bytes/gpu/storage/filesystem/assume_capabilities),
  and `cold_cache: "not_measured"`.
- `version_baseline` with seconds and child peak_memory_bytes for startup context.
- `volumes[]`: status, assets, generator parameters, files, trash, plan_sha256,
  generation_seconds, assets_per_second, library_bytes (allocated file storage),
  library_file_bytes (logical file lengths), and operations. Directory-entry storage is excluded.
- Each measured operation has first_after_generation and statistics (or null); each sample has
  seconds and peak_memory_bytes. Unmeasured operations/volumes contain a reason and no invented data.
- estimated_generation_seconds for larger volumes and estimated_250000_generation_seconds after
  the 10000 baseline. Libraries are independent per volume; results are saved incrementally.

No hostname, username, absolute paths, command lines or hardware serial numbers are persisted.
Measurement errors are failures, never performance thresholds.

## Tests

`perf_gen_smoke` runs in normal Windows/Linux CLI testing without SKIP. It checks the plan hash
and prefix, full verification of 100 assets, all unique payloads, metadata/index samples, interrupted
resume, parameter mismatch immutability, result shape/privacy, statistics and child failure.
`ScaleOpen.TwoHundredAssetsCleanOpenAndListReadNoZip` checks clean RW/RO open/list with before_read=0.
After touching one ZIP it first confirms clean open still reads zero, then marks writer_session=open
via the existing public IndexStore API and observes exactly one ZIP read (owner decision V-1).

`perf_scale` is labelled `periodic`; CI explicitly excludes that label. For routine full validation:

```text
tools\build_cli.cmd --ci -LE periodic
tools\build_cli.cmd --offline --ci -LE periodic
tools/build_linux.sh cli --ci -LE periodic
```

A periodic measurement still needs an explicit storage label and an appropriate optimized build;
the CTest registration uses `unspecified` and writes only to `.cache/perf/scale-result.json`.
