#!/usr/bin/env python3
"""Development-only decoder probe; never a runtime export adapter.

Read the current generator manifest so stale files from a previous run cannot
be counted as supported. A completed probe is not pixel/metadata validation.
The optional full oracle decodes every mask and retains pixel hashes/fields.
That proves decoder acceptance, not equality to a source or printer firmware.
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
    parser.add_argument("--full-oracle", type=Path,
                        help="original UVtools full-decoder development oracle DLL; decode every layer and retain pixel hashes")
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
    if args.full_oracle and not args.full_oracle.is_file():
        parser.error("Full-decoder oracle does not exist")
    logs = directory / ("uvtools-full" if args.full_oracle else "uvtools-current")
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
            probe = (["dotnet", str(args.full_oracle.resolve()), str(executable.parent),
                      "--full-open", str(candidates[0])] if args.full_oracle else
                     command + ["print-properties", str(candidates[0]), "--all"])
            process = subprocess.run(probe,
                                     capture_output=True, text=True, errors="replace",
                                     timeout=args.timeout, env=env)
            output = process.stdout + process.stderr
            result["exit_code"] = process.returncode
            if "DllNotFoundException" in output:
                result["status"] = "blocked_native_dependency"
            elif args.full_oracle:
                (logs / (format_id + ".json")).write_text(process.stdout)
                (logs / (format_id + ".stderr")).write_text(process.stderr)
                try:
                    reports = json.loads(process.stdout)
                    if process.returncode != 0 or not isinstance(reports, list) or len(reports) != 1:
                        raise ValueError("Expected one successful full report")
                    report = reports[0]
                    layers = report["decodedLayers"]
                    if report["status"] != "fully_decoded" or not layers or len(layers) != report["LayerCount"]:
                        raise ValueError("Incomplete full layer decode")
                    for index, layer in enumerate(layers):
                        if (layer["index"] != index or layer["width"] != report["ResolutionX"] or
                            layer["height"] != report["ResolutionY"] or len(layer["sha256"]) != 64 or
                            any(c not in "0123456789abcdef" for c in layer["sha256"])):
                            raise ValueError("Invalid layer dimensions, index or pixel hash")
                    result.update(status="fully_decoded", family=report["family"],
                                  layer_count=len(layers), resolution=[report["ResolutionX"], report["ResolutionY"]])
                except (ValueError, KeyError, TypeError) as error:
                    result.update(status="decode_error", detail=str(error))
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
