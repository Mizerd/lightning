#!/usr/bin/env python3
"""Room-open latency, read out of Lightning's own log.

Answers one question and no other: **how long after a room was opened did
the timeline stop paginating?** That is the number behind "rooms lag when
they load" (docs/open-items.md, 2026-09-05), and before this existed it was
estimated from screenshots and from how long a wait felt.

It measures from `matrix.rust: timeline open` to the LAST
`lightning.timeline.pagination: timeline pagination completed` before the
next open, and reports the page count and the rows those pages actually
added. A settle with many pages and few rows is the MatrixRTC-churn walk:
the filter drops `m.call.member` state before it can become a timeline item,
so a page can cost a second and add nothing.

Two traps this reads around, both of which produced wrong numbers first:

  * `filterOffered` in the Rust log is CUMULATIVE for the whole open, not a
    page size. 27, 47, 67, 127 is four pages of 20, 20, 60, 180 — take the
    delta, or every page after the first looks enormous.
  * A settle is only meaningful against a COLD room. matrix-sdk persists its
    event cache, so the second open of a room reads local disk and is fast
    however slow the first was. Restart, then open the room first.

Usage:

    scripts/room-open-latency.py [path/to/lightning.log]

Produce the log with `--log-file`, which every platform supports:

    ./build-rust/lightning-matrix --backend=rust --log-file /tmp/lightning.log

The log carries no message bodies, no room names (the `room=` field prints
the server name only) and no identifiers beyond that, so it is safe to keep
and to quote from.
"""

import re
import sys
from datetime import datetime

DEFAULT_LOG = "/tmp/lightning.log"
TIMESTAMP = re.compile(r"^(\d{4}-\d\d-\d\dT[\d:.]+Z)")
ADDED = re.compile(r"added= (\d+)")


def timestamp(line):
    match = TIMESTAMP.match(line)
    if match is None:
        return None
    return datetime.strptime(match.group(1), "%Y-%m-%dT%H:%M:%S.%fZ")


def main(path):
    opens = []
    events = []
    try:
        handle = open(path, errors="replace")
    except OSError as error:
        print("cannot read %s: %s" % (path, error), file=sys.stderr)
        return 2
    with handle:
        for line in handle:
            stamp = timestamp(line)
            if stamp is None:
                # Continuation of a multi-line message; it carries no time of
                # its own and nothing here needs it.
                continue
            if "timeline open room=" in line:
                opens.append((stamp, len(events)))
            events.append((stamp, line))

    if not opens:
        print("no room opens in %s — was the log written by a run that "
              "opened one?" % path)
        return 1

    for index, (opened, first) in enumerate(opens):
        last_index = opens[index + 1][1] if index + 1 < len(opens) else len(events)
        settled, pages, rows = opened, 0, 0
        for stamp, line in events[first:last_index]:
            if "timeline pagination completed" in line:
                pages += 1
                settled = stamp
                match = ADDED.search(line)
                if match:
                    rows += int(match.group(1))
        print("open %s  settle %6.0f ms  pages %2d  addedRows %3d"
              % (opened.strftime("%H:%M:%S"),
                 (settled - opened).total_seconds() * 1000, pages, rows))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_LOG))
