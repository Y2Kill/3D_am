"""K-112: deterministic plan, real Catalog writes, resume and process runner."""
import argparse
import getpass
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import sqlite3
import subprocess
import sys
import uuid
import zipfile

PLAN = "ff6dad951f904b063f1388ad1fa39b3c0764bfb75ec26af3b4ddf6ab5f23f767"


def command(args, expected=0):
    result = subprocess.run([str(a) for a in args], capture_output=True, encoding="utf-8", check=False)
    assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
    return json.loads(result.stdout) if expected == 0 else result


def snapshot(root):
    return {str(p.relative_to(root)): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
            for p in root.rglob("*") if p.is_file()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--am", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--build-type", required=True)
    args = parser.parse_args()
    work = args.root / str(uuid.uuid4())
    work.mkdir(parents=True)
    library = work / "library"
    base = [args.generator, "--out", library, "--count", 100, "--seed", 1, "--json"]
    assume = ["--assume-capabilities"] if sys.platform.startswith("linux") else []
    plan = command([*base, "--plan-only"])
    assert not library.exists() and not Path(str(library)+".gen.json").exists()
    assert plan["plan_sha256"] == PLAN
    other = command([*base, "--plan-only", "--seed", 2])
    assert other["plan_sha256"] != PLAN
    extended = command([*base, "--plan-only", "--count", 101])
    assert extended["asset_sha256"][:100] == plan["asset_sha256"]
    stopped = command([*base, *assume, "--stop-after", 7])
    assert stopped["created"] == 7 and stopped["assets"] == 7
    generation = command([*base, *assume])
    assert generation["created"] == 93 and generation["skipped"] == 7
    assert generation["assets"] == 100 and generation["trash"] == 1
    assert generation["plan_sha256"] == PLAN
    full = command([args.am, "library", "verify", library, "--full", "--json"])
    assert full["total"] == 100 and full["checked"] == 100

    actual_hashes, payload_hashes = [], set()
    manifests = {}
    for path in library.rglob("*.zip"):
        with zipfile.ZipFile(path) as archive:
            manifest = json.loads(archive.read("manifest.json"))
            asset = manifest["asset"]
            metadata = asset["metadata"]
            files = []
            for entry in manifest["files"]:
                payload = archive.read(entry["storage_path"])
                digest = hashlib.sha256(payload).hexdigest()
                assert digest == entry["sha256"] and digest not in payload_hashes
                payload_hashes.add(digest)
                files.append({"path": entry["storage_path"].split("/", 2)[2], "file_id": entry["id"],
                              "size": len(payload), "sha256": digest})
            logical = {"asset_id": asset["id"], "op_id": manifest["history"]["entries"][0]["operation_id"],
                       "input_id": manifest["inputs"][0]["id"], "metadata": metadata,
                       "trash": "trash" in path.relative_to(library).parts, "files": files}
            canonical = json.dumps(logical, ensure_ascii=False, separators=(",", ":")) + "\n"
            actual_hashes.append(hashlib.sha256(canonical.encode("utf-8")).hexdigest())
            manifests[asset["id"]] = (metadata, len(files), logical["trash"])
    # This also compares the interrupted generation to the complete continuous plan.
    assert sorted(actual_hashes) == sorted(plan["asset_sha256"])
    assert len(payload_hashes) == generation["files"]
    with sqlite3.connect(f"file:{(library / '.am/index.sqlite').as_posix()}?mode=ro", uri=True) as db:
        assert db.execute("SELECT count(*) FROM assets").fetchone()[0] == 100
        for asset_id, (metadata, file_count, trash) in list(manifests.items())[:10]:
            row = db.execute("SELECT title,file_count,in_trash,manifest_json FROM assets WHERE asset_id=?", (asset_id,)).fetchone()
            assert row[:3] == (metadata["title"], file_count, int(trash))
            assert json.loads(row[3])["asset"]["metadata"] == metadata
            tags = {r[0] for r in db.execute("SELECT tag FROM asset_tags WHERE asset_id=?", (asset_id,))}
            assert tags == set(metadata["tags"])
            categories = {r[0] for r in db.execute("SELECT path FROM asset_categories WHERE asset_id=?", (asset_id,))}
            assert {tuple(json.loads(path)) for path in categories} == {tuple(c["path"]) for c in metadata["categories"]}
    resumed = command([*base, *assume])
    assert resumed["created"] == 0 and resumed["skipped"] == 100
    before = snapshot(work)
    for option, value in [("--seed", 2), ("--content-scale", 2)]:
        failed = command([*base, *assume, option, value], expected=1)
        assert "parameters differ" in failed.stderr
        assert snapshot(work) == before

    runner = Path(__file__).with_name("run_scale.py")
    generation_report = work / "generation.json"
    generation_report.write_text(json.dumps(generation), encoding="utf-8")
    output = work / "result.json"
    run = subprocess.run([sys.executable, "-I", str(runner), "--am", str(args.am), "--generator", str(args.generator),
                          "--root", str(work), "--library", str(library), "--generation-report", str(generation_report),
                          "--counts", "100", "--repeat", "1", "--storage", "synthetic-test", "--build-type", args.build_type,
                          "--output", str(output), *assume], capture_output=True, encoding="utf-8")
    assert run.returncode == 0, (run.stdout, run.stderr)
    raw = output.read_text(encoding="utf-8")
    result = json.loads(raw)
    assert result["schema"] == "am.perf.scale.v1" and result["cold_cache"] == "not_measured"
    assert {"date", "program", "environment", "repeat", "volumes", "version_baseline"} <= result.keys()
    assert {"os", "os_version", "cpu", "logical_cores", "ram_bytes", "gpu", "storage", "filesystem", "assume_capabilities"} <= result["environment"].keys()
    for forbidden in (platform.node(), getpass.getuser(), str(library.resolve()), str(library.resolve()).replace("\\", "\\\\")):
        assert not forbidden or forbidden.casefold() not in raw.casefold(), "Private environment value leaked"
    for name, operation in result["volumes"][0]["operations"].items():
        if sys.platform.startswith("linux") and name in ("sync", "rebuild"):
            assert operation == {"status": "not_measured", "reason": "storage_read_only"}
        else:
            assert operation["status"] == "measured" and operation["statistics"] is None
            assert operation["first_after_generation"]["peak_memory_bytes"] > 0
    spec = importlib.util.spec_from_file_location("scale_runner", runner)
    sys.dont_write_bytecode = True
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    summary = module.stats([{"seconds": s, "peak_memory_bytes": 100} for s in (3, 1, 4, 2)])
    assert (summary["median_seconds"], summary["p95_seconds"], summary["min_seconds"], summary["p95_method"]) == (2.5, 4, 1, "maximum_small_sample")
    try:
        module.measured([sys.executable, "-I", "-c", "raise SystemExit(7)"], work)
    except module.MeasurementError:
        pass
    else:
        raise AssertionError("A nonzero child exit was accepted as measurement data")
    # Check the executable script's exit status as well (a real am refusal).
    Path(str(work / "missing") + ".gen.json").write_text(json.dumps(generation["parameters"]), encoding="utf-8")
    failed = subprocess.run([sys.executable, "-I", str(runner), "--am", str(args.am), "--generator", str(args.generator),
                             "--root", str(work), "--library", str(work / "missing"), "--generation-report", str(generation_report),
                             "--counts", "100", "--repeat", "1", "--storage", "synthetic-test", "--build-type", args.build_type,
                             "--output", str(work / "failed.json")], capture_output=True)
    assert failed.returncode != 0 and b"Child exited with code" in failed.stderr and not (work / "failed.json").exists()
    print("perf_gen_smoke: PASS (plan, full verify, metadata, unique payloads, resume, mismatch, runner/privacy/failure)")


if __name__ == "__main__":
    main()
