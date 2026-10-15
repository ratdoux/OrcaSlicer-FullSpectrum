"""Run one isolated Windows CLI slice and record wall/CPU time and process memory.

Use separate output directories and preserved Release runtimes for each revision.
The input project is read without modification; its embedded settings are used.
"""

import argparse
import ctypes
from ctypes import wintypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


class ProcessMemory(ctypes.Structure):
    _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
        (name, ctypes.c_size_t)
        for name in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
            "PagefileUsage", "PeakPagefileUsage", "PrivateUsage",
        )
    ]


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--plate", type=int, default=1)
    parser.add_argument("--debug", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--info-only", action="store_true")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This process-counter implementation requires Windows.")
    exe, project, output = (p.resolve() for p in (args.exe, args.project, args.output))
    if not exe.is_file() or not project.is_file():
        parser.error("Executable and project must exist.")
    output.mkdir(parents=True, exist_ok=False)
    (output / "data").mkdir()
    command = [str(exe)] + (["--info"] if args.info_only else ["--slice", str(args.plate)])
    command += ["--allow-newer-file", "--mtcpp", "20000000", "--mstpp", str(args.timeout),
                "--debug", str(args.debug), "--datadir", str(output / "data"),
                "--outputdir", str(output), str(project)]
    memory_api = ctypes.WinDLL("psapi").GetProcessMemoryInfo
    memory_api.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemory), wintypes.DWORD]
    memory_api.restype = wintypes.BOOL
    times_api = ctypes.WinDLL("kernel32").GetProcessTimes
    times_api.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    times_api.restype = wintypes.BOOL
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    record = {
        "started": datetime.datetime.now().astimezone().isoformat(),
        "command": command, "project_sha256": sha256(project),
        "dll_sha256": sha256(exe.parent / "Snapmaker_Orca.dll"),
        "local_z_validation": os.environ.get("ORCA_LOCAL_Z_VALIDATE") == "1",
        "local_z_profile": os.environ.get("ORCA_LOCAL_Z_PROFILE", ""),
        "local_z_capture": os.environ.get("ORCA_LOCAL_Z_CAPTURE", ""),
        "local_z_clip": os.environ.get("ORCA_LOCAL_Z_CLIP", "cpu"),
        "seam_alignment": os.environ.get("ORCA_SEAM_ALIGNMENT", "cache"),
        "wall_support": os.environ.get("ORCA_WALL_SUPPORT", "cache"),
        "shell_volume": os.environ.get("ORCA_SHELL_VOLUME", "cache"),
        "shell_projection": os.environ.get("ORCA_SHELL_PROJECTION", "cache"),
        "paint_islands": os.environ.get("ORCA_PAINT_ISLANDS", "cpu"),
        "seam_neighbors_prefetch": os.environ.get("ORCA_SEAM_NEIGHBORS_PREFETCH", "seeds"),
        "polygon_compute": os.environ.get("ORCA_POLYGON_COMPUTE", "cpu"),
        "polygon_stages": os.environ.get("ORCA_POLYGON_STAGES", ""),
        "seam_visibility": os.environ.get("ORCA_SEAM_VISIBILITY", "cpu"),
        "vulkan_device": os.environ.get("ORCA_VULKAN_DEVICE", ""),
        "logical_processors": os.cpu_count(), "samples": [],
    }
    with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
        started = time.perf_counter()
        process = subprocess.Popen(command, cwd=exe.parent, stdout=stdout, stderr=stderr,
                                   startupinfo=startup, creationflags=subprocess.CREATE_NO_WINDOW)
        record["pid"] = process.pid
        print(json.dumps({"pid": process.pid, "output": str(output)}), flush=True)
        peaks = {"working_set": 0, "private_sampled": 0, "commit": 0}
        timed_out = False
        while process.poll() is None:
            memory = ProcessMemory()
            memory.cb = ctypes.sizeof(memory)
            if memory_api(int(process._handle), ctypes.byref(memory), memory.cb):
                elapsed = time.perf_counter() - started
                peaks["working_set"] = max(peaks["working_set"], memory.PeakWorkingSetSize)
                peaks["private_sampled"] = max(peaks["private_sampled"], memory.PrivateUsage)
                peaks["commit"] = max(peaks["commit"], memory.PeakPagefileUsage)
                record["samples"].append([round(elapsed, 3), memory.WorkingSetSize, memory.PrivateUsage])
            if time.perf_counter() - started > args.timeout + 120:
                timed_out = True
                process.kill()
                break
            time.sleep(0.5)
        record["exit_code"] = process.wait()
        record["wall_seconds"] = time.perf_counter() - started
        record["timed_out"] = timed_out
        creation, exit_time, kernel, user = (wintypes.FILETIME() for _ in range(4))
        if times_api(int(process._handle), *(ctypes.byref(v) for v in (creation, exit_time, kernel, user))):
            seconds = lambda v: ((v.dwHighDateTime << 32) | v.dwLowDateTime) / 10000000
            record["cpu_seconds"] = seconds(kernel) + seconds(user)
        record["peak_bytes"] = peaks
    result = output / "result.json"
    if result.exists():
        record["slicer_result"] = json.loads(result.read_text(encoding="utf-8-sig"))
    record["gcodes"] = [{"file": str(p.relative_to(output)), "bytes": p.stat().st_size,
                         "sha256": sha256(p)} for p in output.rglob("*.gcode")]
    (output / "metrics.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in record.items() if k != "samples"}), flush=True)
    return record["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
