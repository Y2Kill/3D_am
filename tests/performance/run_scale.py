"""K-112 process measurements. Standard library only; no performance thresholds."""
import argparse
import ctypes
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile


class MeasurementError(RuntimeError):
    pass


def measured(command, work):
    """Capture the child's own lifetime peak, never aggregate previous children."""
    import time
    with tempfile.TemporaryFile(dir=work) as output, tempfile.TemporaryFile(dir=work) as error:
        started = time.perf_counter_ns()
        process = subprocess.Popen([str(x) for x in command], stdout=output, stderr=error)
        if os.name == "nt":
            class Counters(ctypes.Structure):
                _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + [
                    (name, ctypes.c_size_t) for name in (
                        "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                        "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
            process.wait()
            counters = Counters()
            counters.cb = ctypes.sizeof(counters)
            get_memory = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
            get_memory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
            get_memory.restype = ctypes.c_int
            # Popen.wait does not close _handle. Read before explicitly closing it.
            try:
                if not get_memory(int(process._handle), ctypes.byref(counters), counters.cb):
                    raise ctypes.WinError(ctypes.get_last_error())
                peak = counters.PeakWorkingSetSize
            finally:
                process._handle.Close()
        else:
            _, status, usage = os.wait4(process.pid, 0)
            process.returncode = os.waitstatus_to_exitcode(status)
            peak = int(usage.ru_maxrss) * 1024  # Linux reports KiB.
        elapsed = (time.perf_counter_ns() - started) / 1e9
        output.seek(0)
        if process.returncode:
            # Do not persist CLI diagnostics: they can contain absolute paths.
            raise MeasurementError(f"Child exited with code {process.returncode}")
        return {"seconds": elapsed, "peak_memory_bytes": peak}, output.read()


def stats(samples):
    if not samples:
        return None
    times = sorted(s["seconds"] for s in samples)
    rank = math.ceil(0.95 * len(times))
    return {"samples": samples, "median_seconds": statistics.median(times),
            "p95_seconds": times[rank - 1], "min_seconds": times[0],
            "p95_method": "maximum_small_sample" if rank == len(times) else "nearest_rank",
            "peak_memory_bytes": max(s["peak_memory_bytes"] for s in samples)}


def environment(root, storage, assume):
    cpu, ram, filesystem = platform.machine(), None, None
    if os.name == "nt":
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
        class Memory(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
                (name, ctypes.c_ulonglong) for name in ("total", "available", "page_total", "page_available", "virtual_total", "virtual_available", "extended")]
        memory = Memory()
        memory.length = ctypes.sizeof(memory)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        if not kernel.GlobalMemoryStatusEx(ctypes.byref(memory)):
            raise ctypes.WinError(ctypes.get_last_error())
        ram = memory.total
        volume = ctypes.create_unicode_buffer(1024)
        if not kernel.GetVolumePathNameW(str(root.resolve()), volume, len(volume)):
            raise ctypes.WinError(ctypes.get_last_error())
        fsname = ctypes.create_unicode_buffer(64)
        if not kernel.GetVolumeInformationW(volume.value, None, 0, None, None, None, fsname, len(fsname)):
            raise ctypes.WinError(ctypes.get_last_error())
        filesystem = fsname.value
    else:
        ram = os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
        mounts = []
        for line in Path("/proc/mounts").read_text().splitlines():
            fields = line.split()
            mount = fields[1].replace(r"\040", " ")
            if root.resolve().is_relative_to(mount):
                mounts.append((len(mount), fields[2]))
        filesystem = max(mounts)[1] if mounts else "unknown"
    return {"os": platform.system(), "os_version": platform.version(), "cpu": cpu, "logical_cores": os.cpu_count(),
            "ram_bytes": ram, "gpu": None, "storage": storage, "filesystem": filesystem,
            "assume_capabilities": assume}


def operations(am, library, repeat, work):
    result = {}
    commands = [("version", [am, "version"])] + [(name, [am, "library", name, library, "--json"])
                for name in ("info", "list", "sync", "verify", "rebuild")]
    for name, command in commands:
        # Owner decision V-2: production Linux CLI has no capability override.
        if sys.platform.startswith("linux") and name in ("sync", "rebuild"):
            result[name] = {"status": "not_measured", "reason": "storage_read_only"}
            continue
        print(f"Measuring {name}: {repeat} processes", file=sys.stderr, flush=True)
        samples = [measured(command, work)[0] for _ in range(repeat)]
        result[name] = {"status": "measured", "first_after_generation": samples[0], "statistics": stats(samples[1:])}
    return result


def disk_bytes(library):
    """Allocated file bytes, plus logical lengths for transparent comparison."""
    allocated = logical = 0
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        create = kernel.CreateFileW
        create.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p,
                           ctypes.c_ulong, ctypes.c_ulong, ctypes.c_void_p]
        create.restype = ctypes.c_void_p
        info = kernel.GetFileInformationByHandleEx
        info.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_ulong]
        close = kernel.CloseHandle
        close.argtypes = [ctypes.c_void_p]
        class StandardInfo(ctypes.Structure):
            _fields_ = [("allocation", ctypes.c_longlong), ("length", ctypes.c_longlong),
                        ("links", ctypes.c_ulong), ("delete_pending", ctypes.c_ubyte), ("directory", ctypes.c_ubyte)]
    for path in library.rglob("*"):
        if path.is_file():
            stat = path.stat()
            logical += stat.st_size
            if os.name == "nt":
                handle = create(str(path.resolve()), 0x80, 7, None, 3, 0, None)  # READ_ATTRIBUTES, shared, OPEN_EXISTING
                if handle == ctypes.c_void_p(-1).value:
                    raise ctypes.WinError(ctypes.get_last_error())
                try:
                    standard = StandardInfo()
                    if not info(handle, 1, ctypes.byref(standard), ctypes.sizeof(standard)):  # FileStandardInfo
                        raise ctypes.WinError(ctypes.get_last_error())
                    allocated += standard.allocation
                finally:
                    close(handle)
            else:
                allocated += stat.st_blocks * 512
    return allocated, logical


def run(args):
    args.root.mkdir(parents=True, exist_ok=True)
    version_metric, version = measured([args.am, "version"], args.root)
    version = version.decode("utf-8").strip()
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=Path(__file__).resolve().parents[2],
                            capture_output=True, text=True, check=True).stdout.strip()
    if len(commit) != 40 or any(c not in "0123456789abcdef" for c in commit):
        raise MeasurementError("Invalid program commit")
    result = {"schema": "am.perf.scale.v1", "date": datetime.now(timezone.utc).isoformat(),
              "program": {"version": version, "commit": commit, "build_type": args.build_type},
              "environment": environment(args.root, args.storage, args.assume_capabilities),
              "cold_cache": "not_measured", "repeat": args.repeat, "volumes": [],
              "version_baseline": version_metric}
    speed_10k = None
    for count in args.counts:
        estimate = count / speed_10k if speed_10k else None
        if count > 10000 and (estimate is None or estimate > 3 * 60 * 60):
            result["volumes"].append({"assets": count, "status": "not_measured", "reason": "generation_estimate_exceeds_3h" if estimate else "missing_10000_baseline", "estimated_generation_seconds": estimate})
            continue
        library = args.library or args.root / f"library-{count}"
        command = [args.generator, "--out", library, "--count", count, "--seed", args.seed, "--json"]
        if args.assume_capabilities:
            command.append("--assume-capabilities")
        if args.library:
            generation = json.loads(args.generation_report.read_text(encoding="utf-8"))
            descriptor = json.loads(Path(str(library) + ".gen.json").read_text(encoding="utf-8"))
            if descriptor != generation["parameters"] or descriptor["count"] != count:
                raise MeasurementError("Generation report does not match the library descriptor")
        else:
            print(f"Generating {count} assets", file=sys.stderr, flush=True)
            _, output = measured(command, args.root)
            generation = json.loads(output)
        if not args.library and generation["created"] != count:
            raise MeasurementError("Measurement requires a freshly generated volume; choose a new --root")
        if count == 10000:
            speed_10k = generation["assets_per_second"]
        print(f"Generated {count}: {generation['assets_per_second']:.3f} assets/s", file=sys.stderr, flush=True)
        allocated, logical = disk_bytes(library)
        volume = {"status": "measured", "assets": count, "parameters": generation["parameters"],
                  "files": generation["files"], "trash": generation["trash"],
                  "generation_seconds": generation["generation_seconds"], "assets_per_second": generation["assets_per_second"],
                  "plan_sha256": generation["plan_sha256"],
                  "library_bytes": allocated, "library_file_bytes": logical,
                  "estimated_generation_seconds": estimate,
                  "operations": operations(args.am, library, args.repeat, args.root)}
        result["volumes"].append(volume)
        # Save completed volumes before starting a potentially long next volume.
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if speed_10k:
        result["estimated_250000_generation_seconds"] = 250000 / speed_10k
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--am", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--root", type=Path, default=Path(".cache/perf/scale"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--storage", required=True, help="Explicit operator label, e.g. nvme-ssd; never inferred")
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--counts", type=int, nargs="+", default=[100, 1000, 10000, 50000, 100000, 250000])
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--assume-capabilities", action="store_true")
    parser.add_argument("--library", type=Path, help="Measure an already generated volume (single --counts value)")
    parser.add_argument("--generation-report", type=Path, help="Generator JSON for --library")
    args = parser.parse_args()
    if args.repeat < 1 or any(c < 1 or c > 250000 for c in args.counts) or args.counts != sorted(set(args.counts)):
        parser.error("Positive repeat and unique ascending counts in 1..250000 are required")
    if bool(args.library) != bool(args.generation_report) or (args.library and len(args.counts) != 1):
        parser.error("--library requires --generation-report and exactly one count")
    try:
        run(args)
    except (MeasurementError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"run_scale: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
