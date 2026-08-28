"""Runs the measurement phases. Exit code is the verdict.

Mirrors cpp/bin/solution and go/bin/weir. Invariants that are fast and want a
test runner live in pytest instead; that split is deliberate and is explained in
solution.md section 10.
"""

from __future__ import annotations

import argparse
import sys

from . import phases
from .harness import failures


def main() -> int:
    ap = argparse.ArgumentParser(prog="weir_bench")
    ap.add_argument("--short", action="store_true",
                    help="reduced event counts, for a quick check")
    ap.add_argument("--phase", default="",
                    help="run one of: env, gil, throughput, backpressure, scanab, baselines")
    args = ap.parse_args()

    def run(name: str) -> bool:
        return not args.phase or args.phase == name

    if run("env"):
        phases.environment()
    if run("gil"):
        phases.gil_sweep(args.short)
    if run("throughput"):
        phases.throughput(args.short)
    if run("backpressure"):
        phases.backpressure(args.short)
    if run("scanab"):
        phases.scan_ab(args.short)
    if run("baselines"):
        phases.baselines(args.short)

    n = failures()
    if n == 0:
        print("all checks passed")
        return 0
    print(f"{n} checks FAILED")
    return min(n, 125)


if __name__ == "__main__":
    sys.exit(main())
