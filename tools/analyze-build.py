#!/usr/bin/env python3
"""Summarise where a Lightning build spent its time, from Ninja's own log.

Reads <build-dir>/.ninja_log, which Ninja rewrites with the duration of every
edge it runs. Nothing here modifies the build tree, and the log is only read.

    tools/analyze-build.py build-rust
    tools/analyze-build.py build-rust --top 40

The numbers are CPU time summed over edges, not wall-clock: a build running at
-j18 finishes in roughly (total / 18) plus whatever cannot be parallelised.
Only the LAST recorded run of each output is counted.

IMPORTANT: Ninja never forgets. The log keeps an entry for every output it has
ever built in this directory, including ones the current build graph no longer
contains. After a change to the build graph the totals therefore describe a
mixture of the old and new shapes, and will keep reporting work that is no
longer done. Reconfiguring does not clear it. To measure a changed graph
honestly, either time a real build, or start from a fresh build directory.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys

OBJECT_RE = re.compile(r"CMakeFiles/(?P<target>[^/]+)\.dir/(?P<source>.*)\.o$")


def read_log(path: pathlib.Path) -> dict[str, int]:
    """Map each output to the duration in ms of the last run that produced it."""
    durations: dict[str, int] = {}
    with path.open() as handle:
        for line in handle:
            if line.startswith("#"):
                continue
            fields = line.rstrip("\n").split("\t")
            if len(fields) < 5:
                continue
            try:
                start, end = int(fields[0]), int(fields[1])
            except ValueError:
                continue
            durations[fields[3]] = end - start
    return durations


def minutes(ms: int) -> str:
    return f"{ms / 60000:.1f}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument("--top", type=int, default=20,
                        help="how many rows to show in each table (default 20)")
    args = parser.parse_args()

    log = args.build_dir / ".ninja_log"
    if not log.is_file():
        print(f"no .ninja_log in {args.build_dir} — has anything been built there?",
              file=sys.stderr)
        return 1

    durations = read_log(log)
    if not durations:
        print(f"{log} contains no completed edges", file=sys.stderr)
        return 1

    objects: dict[str, int] = {}
    others: dict[str, int] = {}
    for output, ms in durations.items():
        (objects if output.endswith(".o") else others)[output] = ms

    by_source: collections.Counter[str] = collections.Counter()
    copies: collections.Counter[str] = collections.Counter()
    by_target: collections.Counter[str] = collections.Counter()
    for output, ms in objects.items():
        match = OBJECT_RE.match(output)
        key = match.group("source") if match else output
        by_source[key] += ms
        copies[key] += 1
        if match:
            by_target[match.group("target")] += ms

    total = sum(durations.values())
    compile_total = sum(objects.values())
    link_total = sum(ms for out, ms in others.items()
                     if out.endswith((".a", ".so")) or "/" not in out)

    print(f"{log}: {len(durations)} edges")
    print("  NOTE: this log is cumulative. Outputs the current build graph no")
    print("        longer produces are still counted, so after a build-graph")
    print("        change these totals describe the old and new shapes mixed.")
    print(f"  total      {minutes(total):>7} CPU-min")
    print(f"  compiling  {minutes(compile_total):>7} CPU-min  ({len(objects)} objects)")
    print(f"  linking    {minutes(link_total):>7} CPU-min")
    print(f"  other      {minutes(total - compile_total - link_total):>7} CPU-min"
          "  (generated code, CMake regeneration, QML tooling)")

    print(f"\nSlowest translation units, aggregated over every target that compiles them")
    print(f"{'CPU-min':>8} {'x':>4} {'avg s':>7}  source")
    for source, ms in by_source.most_common(args.top):
        print(f"{minutes(ms):>8} {copies[source]:>4} "
              f"{ms / copies[source] / 1000:>7.1f}  {source}")

    duplicated = [(s, n, ms) for s, ms in by_source.items() if (n := copies[s]) > 1]
    if duplicated:
        wasted = sum(ms - ms // n for _, n, ms in duplicated)
        worst = max(duplicated, key=lambda row: row[2])
        print(f"\n{len(duplicated)} sources are compiled by more than one target.")
        print(f"  Compiling each of them once would save about {minutes(wasted)} CPU-min.")
        print(f"  Most duplicated: {worst[0]} ({worst[1]} copies)")

    print(f"\nMost expensive targets")
    print(f"{'CPU-min':>8} {'objs':>6}  target")
    for target, ms in by_target.most_common(args.top):
        objs = sum(1 for out in objects if OBJECT_RE.match(out)
                   and OBJECT_RE.match(out).group("target") == target)
        print(f"{minutes(ms):>8} {objs:>6}  {target}")

    slow_links = sorted(((ms, out) for out, ms in others.items() if ms > 5000),
                        reverse=True)[:args.top]
    if slow_links:
        print(f"\nSlowest non-compile edges")
        for ms, out in slow_links:
            print(f"{ms / 1000:>8.1f}s  {out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
