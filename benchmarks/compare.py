#!/usr/bin/env python3
"""Paired, same-host benchmark comparison. No third-party Python dependencies."""
import argparse
import json
import math
import os
import platform
import statistics
import subprocess
import tempfile
from pathlib import Path


def summarize(samples):
    median = statistics.median(samples)
    return {
        "median_ns": median,
        "p95_run_ns": sorted(samples)[math.ceil(len(samples) * 0.95) - 1],
        "relative_mad": statistics.median(abs(v - median) for v in samples) / median,
        "samples_ns": samples,
    }


def compare(base_runs, candidate_runs, threshold=0.10, noise_limit=0.10):
    if len(base_runs) < 5 or len(base_runs) != len(candidate_runs):
        raise ValueError("at least five paired runs are required")
    names = set(base_runs[0])
    for run in base_runs:
        if set(run) != names:
            raise ValueError("baseline case inventory changed across runs")
    for run in candidate_runs:
        if not names.issubset(run):
            raise ValueError("candidate removed a baseline benchmark")
    results = []
    for name in sorted(names):
        old = [r[name] for r in base_runs]
        new = [r[name] for r in candidate_runs]
        base, candidate = summarize(old), summarize(new)
        delta = candidate["median_ns"] / base["median_ns"] - 1
        slower = sum(n > b * (1 + threshold) for b, n in zip(old, new))
        noisy = max(base["relative_mad"], candidate["relative_mad"]) > noise_limit
        status = "noisy" if noisy else (
            "regression" if delta > threshold and slower >= math.ceil(len(old) * 0.8) else "pass")
        results.append({"name": name, "baseline": base, "candidate": candidate,
                        "change": delta, "slower_pairs": slower, "status": status})
    return results


def measure(binary, iterations, output, raw_reports=None):
    subprocess.run([str(binary), "--iterations", str(iterations), "--json", str(output)],
                   check=True, stdout=subprocess.DEVNULL)
    data = json.loads(output.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise ValueError("unsupported benchmark JSON schema")
    rows = {}
    for item in data["results"]:
        name, value = item["name"], item["nanoseconds_per_operation_median"]
        if name in rows or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError("duplicate or invalid benchmark result")
        rows[name] = value
    if not rows:
        raise ValueError("empty benchmark result")
    if raw_reports is not None:
        raw_reports.append(data)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline-ref", default="unknown")
    parser.add_argument("--candidate-ref", default="working-tree")
    parser.add_argument("--iterations", type=int, default=100000)
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--threshold", type=float, default=0.10)
    parser.add_argument("--noise-limit", type=float, default=0.10)
    parser.add_argument("--output", type=Path, default=Path("benchmark-comparison.json"))
    parser.add_argument("--markdown", type=Path, default=Path("benchmark-comparison.md"))
    args = parser.parse_args()
    if args.runs < 5 or args.iterations < 1000 or not 0 < args.threshold < 1 or not 0 < args.noise_limit < 1:
        parser.error("require >=5 runs, >=1000 iterations, threshold/noise-limit in (0,1)")
    binaries = [args.baseline.resolve(), args.candidate.resolve()]
    runs = [[], []]
    raw_reports = [[], []]
    with tempfile.TemporaryDirectory(prefix="sqlconduit-bench-") as tmp:
        for pair in range(args.runs):
            order = [0, 1] if pair % 2 == 0 else [1, 0]
            measured = {}
            for index in order:
                measured[index] = measure(binaries[index], args.iterations,
                                          Path(tmp) / f"{pair}-{index}.json", raw_reports[index])
            for index in (0, 1):
                runs[index].append(measured[index])
    results = compare(*runs, args.threshold, args.noise_limit)
    report = {
        "schema_version": 1, "host": platform.platform(), "cpu": platform.processor(),
        "cpu_count": os.cpu_count(), "baseline_ref": args.baseline_ref,
        "candidate_ref": args.candidate_ref, "runs": args.runs, "iterations": args.iterations,
        "threshold": args.threshold, "noise_limit": args.noise_limit, "results": results,
        "raw_baseline_runs": raw_reports[0], "raw_candidate_runs": raw_reports[1],
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    lines = ["# SQLConduit performance comparison", "",
             f"Baseline `{args.baseline_ref}`; candidate `{args.candidate_ref}`; {args.runs} paired runs.", "",
             "| Case | Baseline ns/op | Candidate ns/op | Change | Result |",
             "| --- | ---: | ---: | ---: | --- |"]
    for r in results:
        lines.append(f"| {r['name']} | {r['baseline']['median_ns']:.2f} | "
                     f"{r['candidate']['median_ns']:.2f} | {r['change']:+.1%} | {r['status']} |")
    lines.extend(["", "p95_run_ns measures variation between whole benchmark runs, not individual-operation tail latency.",
                  "Noisy runs fail with exit 2 and need investigation or a quieter rerun. Stable regressions fail with exit 1."])
    args.markdown.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 1 if any(r["status"] == "regression" for r in results) else (
        2 if any(r["status"] == "noisy" for r in results) else 0)


if __name__ == "__main__":
    raise SystemExit(main())
