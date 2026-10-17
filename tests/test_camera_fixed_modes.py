#!/usr/bin/env python3
"""Manual CPU-only camera negotiation check, using gst-launch-1.0.

Run inside the development shell. Only synthetic sources and fakesink are
opened. Extract production entry strings so a hard source ceiling cannot
silently diverge from this check. Qt tests cover advertised-mode selection
and the complete encoded pipeline; this check requires no build.

Mixed-mode cases use explicit expected selector-result fixtures, not a Python
copy of the selector. Verbose capture-pad caps distinguish real 30 fps input
from the 30 fps output videorate can produce by duplicating 5 fps input.
"""
import ast
import re
import shlex
import subprocess
from pathlib import Path


def literals(body):
    pattern = r'QStringLiteral\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)'
    return [
        "".join(ast.literal_eval(token) for token in
                re.findall(r'"(?:[^"\\]|\\.)*"', match.group(1)))
        for match in re.finditer(pattern, body)
    ]


def main():
    source = (Path(__file__).resolve().parents[1]
              / "src/calls/SfuMediaEngine.cpp").read_text()

    def body(name, next_name):
        return source.split("QString SfuMediaEngine::" + name, 1)[1].split(
            next_name, 1)[0]

    jpeg_entry = literals(body(
        "cameraJpegEntry(", "QString SfuMediaEngine::directCameraRawEntry"))[-1]
    raw_base = literals(body(
        "captureEntryFilter(", "bool SfuMediaEngine::isDirectCameraSource"))[-1]
    raw_entry = literals(body(
        "directCameraRawEntry(", "QString SfuMediaEngine::captureCameraEntry"))[-1]
    rate = literals(body(
        "videoRateStage(", "QString SfuMediaEngine::portalCameraEntry"))[-1]
    old_jpeg = (
        'capsfilter caps="image/jpeg,width=(int)[1,1920],height=(int)[1,1080]" '
        '! jpegdec ! videoconvert ! capsfilter '
        'caps="video/x-raw,pixel-aspect-ratio=(fraction)1/1"')
    old_raw = (
        'capsfilter caps="video/x-raw,width=(int)[1,1920],height=(int)[1,1080],'
        'pixel-aspect-ratio=(fraction)1/1"')
    results = []
    for jpeg in (False, True):
        for width, height in ((3840, 2160), (1920, 1080)):
            raw = (f"video/x-raw,format=I420,width={width},height={height},"
                   "framerate=60/1,pixel-aspect-ratio=1/1")
            mode = (f"image/jpeg,width={width},height={height},framerate=60/1"
                    if jpeg else raw)
            fixed_source = ["videotestsrc", "num-buffers=3", "!", raw]
            if jpeg:
                fixed_source += ["!", "jpegenc", "!", mode]
            entry = (jpeg_entry.replace("%1", mode) if jpeg else
                     raw_entry.replace("%1", mode).replace("%2", raw_base))
            unprobed = jpeg_entry.replace("%1", "image/jpeg") if jpeg else raw_base
            rest = [
                "!", "videoconvert", "!", "videoscale", "!", *shlex.split(rate),
                "!", "video/x-raw,width=1280,height=720,framerate=30/1,"
                "pixel-aspect-ratio=1/1", "!", "fakesink", "sync=false",
            ]
            for version, chosen in [
                ("reviewed", old_jpeg if jpeg else old_raw),
                ("fixed", entry), ("fixed-unprobed", unprobed),
            ]:
                command = ["gst-launch-1.0", "-q", *fixed_source,
                           "!", *shlex.split(chosen), *rest]
                result = subprocess.run(command, capture_output=True,
                                        text=True, timeout=20)
                expect_failure = version == "reviewed" and width > 1920
                passed = ((result.returncode != 0) if expect_failure
                          else result.returncode == 0)
                label = f'{version} {"JPEG" if jpeg else "raw"} {width}x{height}@60'
                print(f'{label}: exit {result.returncode}, expected '
                      f'{"failure" if expect_failure else "EOS"}, '
                      f'{"PASS" if passed else "FAIL"}', flush=True)
                if result.stderr:
                    print(result.stderr.strip(), flush=True)
                results.append(passed)

    # Each mode is width/height/rate. Keep the device's slow large mode first
    # so these checks do not pass just because the source prefers 720p.
    slow = "width=1920,height=1080,framerate=5/1"
    fast = "width=1280,height=720,framerate=30/1"
    sixty = "width=1280,height=720,framerate=60/1"
    full = "width=1920,height=1080,framerate=30/1"
    rate_list = "width=1920,height=1080,framerate=(fraction){5/1,30/1}"
    rate_range = "width=1920,height=1080,framerate=(fraction)[5/1,60/1]"
    fast_range = "width=1920,height=1080,framerate=(fraction)[30/1,60/1]"
    larger_slow = "width=3840,height=2160,framerate=5/1"
    larger_fast = "width=2560,height=1440,framerate=30/1"
    fixtures = [
        ("reviewed-area-only", [slow, fast], [slow], (1920, 1080, 5)),
        ("mixed-rate-fixed", [slow, fast], [fast], (1280, 720, 30)),
        ("mixed-60-only", [slow, sixty], [sixty], (1280, 720, 60)),
        ("rate-list", [rate_list, fast], [full, fast], (1920, 1080, 30)),
        ("rate-range", [rate_range, fast], [fast_range, fast], (1920, 1080, 30)),
        # A second size must remain usable if another constraint rules out
        # the first size: the entry still contains BOTH advertised sizes.
        ("retained-alternative", [sixty], [full, sixty], (1280, 720, 60)),
        ("above-1080p-mixed", [larger_slow, larger_fast], [larger_fast],
         (2560, 1440, 30)),
    ]

    def caps_for(type_name, modes, raw=False):
        extra = ",format=I420,pixel-aspect-ratio=1/1" if raw else ""
        return ";".join(type_name + extra + "," + mode for mode in modes)

    for jpeg in (False, True):
        for label, source_modes, selected_modes, expected in fixtures:
            source_caps = caps_for("video/x-raw", source_modes, raw=True)
            type_name = "image/jpeg" if jpeg else "video/x-raw"
            selected = caps_for(type_name, selected_modes, raw=not jpeg)
            entry = (jpeg_entry.replace("%1", selected) if jpeg else
                     raw_entry.replace("%1", selected).replace("%2", raw_base))
            command = [
                "gst-launch-1.0", "-v", "videotestsrc", "num-buffers=6", "!",
                "capsfilter", "name=syntheticcapture", "caps=" + source_caps,
            ]
            if jpeg:
                command += ["!", "jpegenc"]
            command += [
                "!", *shlex.split(entry), "!", "videoconvert", "!", "videoscale",
                "!", *shlex.split(rate), "!",
                "video/x-raw,width=1280,height=720,framerate=30/1,"
                "pixel-aspect-ratio=1/1", "!", "fakesink", "sync=false",
            ]
            result = subprocess.run(command, capture_output=True,
                                    text=True, timeout=20)
            capture = re.search(
                r"GstCapsFilter:syntheticcapture\.GstPad:src: caps = ([^\n]+)",
                result.stdout)
            actual = None
            if capture:
                fields = capture.group(1)
                width = re.search(r"width=\(int\)(\d+)", fields)
                height = re.search(r"height=\(int\)(\d+)", fields)
                fps = re.search(r"framerate=\(fraction\)(\d+)/(\d+)", fields)
                if width and height and fps:
                    actual = (int(width[1]), int(height[1]),
                              int(fps[1]) / int(fps[2]))
            passed = (result.returncode == 0 and actual == expected
                      and "Got EOS" in result.stdout)
            print(f'{label} {"JPEG" if jpeg else "raw"}: exit '
                  f'{result.returncode}, capture={actual}, expected={expected}, '
                  f'{"PASS" if passed else "FAIL"}', flush=True)
            if not passed:
                print(result.stdout + result.stderr, flush=True)
            results.append(passed)
    print(f"NEGOTIATION: {sum(results)}/{len(results)} expected outcomes, "
          f"{len(results) - sum(results)} unexpected, 0 skipped", flush=True)
    return int(not all(results))


if __name__ == "__main__":
    raise SystemExit(main())
