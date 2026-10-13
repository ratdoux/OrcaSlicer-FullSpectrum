"""Profile one disposable Windows CLI slice with the process-only x64 sampler.

Build tools/profile_slicing_windows.cpp first. Raw data goes under --output;
the existing GUI and source project are not touched. Sampling briefly suspends
active CLI threads, so use separate unprofiled runs for performance comparisons.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--sampler", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--period-ms", type=int, default=20)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    command = [sys.executable, str(Path(__file__).with_name("benchmark_cpu_slicing.py")),
               "--exe", str(args.exe.resolve()), "--project", str(args.project.resolve()),
               "--output", str(output / "slice")]
    with (output / "runner.log").open("w", encoding="utf-8") as runner_log:
        runner = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        first_line = runner.stdout.readline()
        runner_log.write(first_line)
        runner_log.flush()
        try:
            pid = json.loads(first_line)["pid"]
        except (ValueError, KeyError):
            runner_log.write(runner.stdout.read())
            runner.wait()
            raise RuntimeError(f"Benchmark did not start; see {output / 'runner.log'}")
        sampler_command = [str(args.sampler.resolve()), str(pid), str(output / "profile"),
                           str(args.symbols.resolve()), str(args.period_ms)]
        (output / "profile_command.json").write_text(json.dumps(sampler_command, indent=2), encoding="utf-8")
        print(f"Profiling isolated slicing PID {pid}; output {output}", flush=True)
        with (output / "sampler.log").open("wb") as sampler_log:
            sampler = subprocess.Popen(sampler_command, stdout=sampler_log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            runner_log.write(runner.stdout.read())
            runner_code = runner.wait()
            sampler_code = sampler.wait()
    print(json.dumps({"slice_runner_exit": runner_code, "sampler_exit": sampler_code}), flush=True)
    return runner_code or sampler_code


if __name__ == "__main__":
    raise SystemExit(main())
