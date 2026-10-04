#!/usr/bin/env python3
"""Development-only decoder probe; never a runtime export adapter.

Read the current generator manifest so stale files from a previous run cannot
be counted as supported. A completed probe is not pixel/metadata validation.
Only local executables are invoked; nothing is installed or downloaded.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="sla-format-audit directory")
    parser.add_argument("uvtools", type=Path, help="local UVtoolsCmd executable or DLL")
    parser.add_argument("--native-dir", type=Path, help="directory containing libcvextern.so")
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    directory = args.directory.resolve()
    executable = args.uvtools.resolve()
    if not executable.is_file():
        parser.error("UVtools executable does not exist")
    with (directory / "exports.tsv").open(newline="") as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    logs = directory / "uvtools-current"
    logs.mkdir(exist_ok=True)
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT="1",
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1")
    if args.native_dir:
        env["LD_LIBRARY_PATH"] = str(args.native_dir.resolve()) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    command = (["dotnet", str(executable)] if executable.suffix.lower() == ".dll"
               else [str(executable)])
    results = []
    for row in rows:
        format_id = row["format"]
        if not format_id or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" for c in format_id):
            raise ValueError("Invalid format ID in manifest")
        result = {"format": format_id, "export_status": row["status"]}
        results.append(result)
        if row["status"] != "written":
            result.update(status="not_exported", detail=row["detail"])
            continue
        candidates = list(directory.glob("audit-" + format_id + ".*"))
        if len(candidates) != 1:
            result.update(status="fixture_error", detail="Expected exactly one current fixture")
            continue
        try:
            process = subprocess.run(command + ["print-properties", str(candidates[0]), "--all"],
                                     capture_output=True, text=True, errors="replace",
                                     timeout=args.timeout, env=env)
            output = process.stdout + process.stderr
            result["exit_code"] = process.returncode
            if "DllNotFoundException" in output:
                result["status"] = "blocked_native_dependency"
            elif "Unhandled exception" in output:
                result["status"] = "decoder_error"
            else:
                # This UVtools snapshot can return 1 even on success. Never
                # interpret a return code alone as proof of a valid decode.
                result["status"] = "requires_log_review"
        except subprocess.TimeoutExpired:
            output = "UVtools exceeded the configured timeout.\n"
            result["status"] = "timeout"
        except OSError as error:
            output = str(error)
            result["status"] = "invocation_error"
        (logs / (format_id + ".log")).write_text(output)
    report = logs / "results.json"
    report.write_text(json.dumps(results, indent=2) + "\n")
    counts = {}
    for result in results:
        status = result["status"]
        counts[status] = counts.get(status, 0) + 1
    print(json.dumps({"report": str(report), "counts": counts}, indent=2))


if __name__ == "__main__":
    main()
