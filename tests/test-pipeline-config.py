#!/usr/bin/env python3
"""Structural and rule tests for .gitlab-ci.yml.

Proves the pipeline contains the required six-stage graph, that the publish,
verify, and release jobs are gated only on PUBLISH_PACKAGES (so both release
actions include them), that they are correctly wired downstream of validation
and each other, and that a build-only pipeline excludes them entirely.
"""
import os
import re
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.join(HERE, "..", ".gitlab-ci.yml")

errors = []


def check(cond, msg):
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"  FAIL: {msg}", file=sys.stderr)
        errors.append(msg)


with open(CI) as fh:
    doc = yaml.safe_load(fh)

# --- stages ---
required_stages = ["resolve", "build", "validate", "publish", "verify",
                   "sign", "release", "mirror", "update"]
stages = doc["stages"]
idxs = [stages.index(s) for s in required_stages if s in stages]
check(all(s in stages for s in required_stages), "all required stages present")
check(idxs == sorted(idxs), "required stages are in the correct order")

# --- jobs present ---
required_jobs = [
    "resolve-source", "build-deb", "build-rpm",
    "build-flatpak", "build-appimage", "build-snap",
    "validate-deb", "validate-rpm",
    "validate-flatpak", "validate-appimage", "validate-snap",
    "publish-packages", "verify-published-packages", "finalize-release",
    "sign-update-manifest", "mirror-release-to-github", "publish-update-manifest",
    "windows-package-test", "build-windows", "macos-package-test",
]
for job in required_jobs:
    check(job in doc, f"job {job} is defined")

# --- one dedicated runner pool per format: every build/validate job selects
# --- exactly one unique selector tag. Each selector matches a runner on both
# --- the GitLab VM fleet and the mirrored fleet on 10.195.35.6, so whichever
# --- matching runner is free takes the job.
UNIQUE_SELECTORS = {"apt", "dnf", "nix", "flatpak", "appimage", "snap"}
FORMAT_SELECTOR = {
    "deb": "apt", "rpm": "dnf",
    "flatpak": "flatpak", "appimage": "appimage", "snap": "snap",
}
for fmt, selector in FORMAT_SELECTOR.items():
    for prefix in ("build-", "validate-"):
        job = prefix + fmt
        tags = set(doc[job].get("tags", []))
        selectors = tags & UNIQUE_SELECTORS
        check(selectors == {selector},
              f"{job} selects exactly its own runner tag [{selector}]")



def resolve_extends(name):
    """Merge a job with its extends chain (shallow, one dict deep)."""
    job = dict(doc[name])
    parents = job.get("extends")
    if isinstance(parents, str):
        parents = [parents]
    merged = {}
    for p in parents or []:
        if p in doc:
            merged.update(resolve_extends_dict(doc[p]))
    merged.update(job)
    return merged


def resolve_extends_dict(d):
    d = dict(d)
    parents = d.get("extends")
    if isinstance(parents, str):
        parents = [parents]
    merged = {}
    for p in parents or []:
        if p in doc:
            merged.update(resolve_extends_dict(doc[p]))
    merged.update(d)
    return merged


# --- bounded-concurrency invariant: build jobs are split across exactly two
# --- resource groups, so at most TWO package builds run at once anywhere
# --- (one per group). Worst-case co-location on one host is sized for in the
# --- runner caps; do not add a third group without revisiting host capacity.
BUILD_GROUP = {
    "deb": "lightning-package-build-a",
    "flatpak": "lightning-package-build-a",
    "snap": "lightning-package-build-a",
    "rpm": "lightning-package-build-b",
    "appimage": "lightning-package-build-b",
}
for fmt, group in BUILD_GROUP.items():
    merged = resolve_extends("build-" + fmt)
    check(merged.get("resource_group") == group,
          f"build-{fmt} is bounded by resource group {group}")
check(len(set(BUILD_GROUP.values())) == 2,
      "build jobs use exactly two resource groups (bounded 2-way concurrency)")
# Cross-host routing (2026-07-20): the two resource groups are the CI half of
# the "parallel lanes land on different hosts" guarantee. Each host runs at
# most one package job at a time (global concurrent=1 on both the mirror VM
# and the consolidated package-runner-packages manager on the GitLab VM), so
# two lanes able to run at once must split across the two hosts. Both groups
# must be non-empty for the two lanes to exist.
group_members = {}
for fmt, group in BUILD_GROUP.items():
    group_members.setdefault(group, []).append(fmt)
check(all(len(m) >= 1 for m in group_members.values())
      and len(group_members) == 2,
      "both resource groups are non-empty (two lanes exist to distribute)")

# --- publish-chain jobs route API requests through the internal endpoint
# --- (the public host is Cloudflare-proxied with a request-body cap that
# --- large package files exceed) ---
for job in ["publish-packages", "verify-published-packages", "finalize-release",
            "sign-update-manifest", "mirror-release-to-github",
            "publish-update-manifest"]:
    merged = resolve_extends(job)
    base = (merged.get("variables") or {}).get("PUBLISH_API_BASE", "")
    check(base.startswith("http://10.195.35.2"),
          f"{job} sets PUBLISH_API_BASE to the internal GitLab endpoint")

# --- BUILD_JOBS values stay within the runner guard (configure-build.sh
# --- accepts 1-8) ---
default_jobs = str(doc.get("variables", {}).get("BUILD_JOBS", ""))
check(default_jobs.isdigit() and 1 <= int(default_jobs) <= 8,
      "default BUILD_JOBS is within 1-8")
for fmt in FORMAT_SELECTOR:
    jobs = str((doc["build-" + fmt].get("variables") or {}).get("BUILD_JOBS", default_jobs))
    check(jobs.isdigit() and 1 <= int(jobs) <= 8,
          f"build-{fmt} BUILD_JOBS is within 1-8")

# --- workflow limited to web/api ---
wf = doc["workflow"]["rules"]
srcs = " ".join(str(r) for r in wf)
check('"web"' in srcs and '"api"' in srcs and "when: never" in yaml.dump(doc["workflow"]),
      "workflow accepts only web/api pipelines")

publish_jobs = ["publish-packages", "verify-published-packages", "finalize-release",
                "sign-update-manifest", "mirror-release-to-github",
                "publish-update-manifest"]

# --- publish/verify/release rules: gated on PUBLISH_PACKAGES only ---
rule_texts = {}
for job in publish_jobs:
    merged = resolve_extends(job)
    rules = merged.get("rules")
    check(rules is not None, f"{job} has rules")
    txt = yaml.dump(rules)
    rule_texts[job] = txt
    check('PUBLISH_PACKAGES == "true"' in txt, f"{job} rule requires PUBLISH_PACKAGES==true")
    check("CI_DEFAULT_BRANCH" in txt, f"{job} rule requires the default branch")
    check("RELEASE_ACTION" not in txt,
          f"{job} rule does not depend on RELEASE_ACTION (both actions included)")
    check("SOURCE_REF" not in txt,
          f"{job} rule does not gate on SOURCE_REF (create-mode SHAs included)")

check(len(set(rule_texts.values())) == 1,
      "publish/verify/release share one consistent gate")

# --- dependency wiring ---
def needs_names(job):
    ns = doc[job].get("needs", [])
    out = []
    for n in ns:
        out.append(n["job"] if isinstance(n, dict) else n)
    return out

check(set(["validate-deb", "validate-rpm", "validate-flatpak",
           "validate-appimage", "validate-snap"]).issubset(
          needs_names("publish-packages")),
      "publish-packages depends on every format's validation job")
check("build-windows" in needs_names("publish-packages"),
      "publish-packages depends on the Windows build (portable/MSI/setup)")
check("resolve-source" in needs_names("publish-packages"),
      "publish-packages depends on resolve-source")
check("publish-packages" in needs_names("verify-published-packages"),
      "verify-published-packages depends on publish-packages")
check("verify-published-packages" in needs_names("finalize-release"),
      "finalize-release depends on verify-published-packages")
check("publish-packages" in needs_names("finalize-release"),
      "finalize-release depends on publish-packages")

for job in ["validate-deb", "validate-rpm", "validate-flatpak",
            "validate-appimage", "validate-snap"]:
    check("build-" + job.split("-", 1)[1] in needs_names(job),
          f"{job} depends on its build job")

# --- signed update manifest (UPDATE-SPEC) ------------------------------------
# The split across two stages is the whole point of these jobs' placement:
#
#   sign-update-manifest runs BEFORE the release, so a missing or broken signing
#   key fails while nothing irreversible has happened. Discovering it afterwards
#   would leave an immutable tag and release for a version whose update manifest
#   cannot be produced.
#
#   publish-update-manifest runs AFTER the release, because the "latest" slot is
#   what every installed Lightning polls and the manifest's release_notes_url
#   points at the release page. Promoting it earlier would advertise an update
#   whose release does not exist -- and would leave that advertisement standing
#   if finalize-release then failed.
check(doc["sign-update-manifest"].get("stage") == "sign",
      "sign-update-manifest runs in the sign stage")
check(doc["publish-update-manifest"].get("stage") == "update",
      "publish-update-manifest runs in the update stage")
check(stages.index("publish") < stages.index("verify") < stages.index("sign")
      < stages.index("release") < stages.index("mirror") < stages.index("update"),
      "stage order is publish -> verify -> sign -> release -> mirror -> update")
check("verify-published-packages" in needs_names("sign-update-manifest"),
      "sign-update-manifest depends on verify-published-packages "
      "(a manifest is only built from a verified publication)")
check("resolve-source" in needs_names("sign-update-manifest"),
      "sign-update-manifest depends on resolve-source")
check("sign-update-manifest" in needs_names("finalize-release"),
      "finalize-release waits for the manifest to be signed "
      "(no tag is created for a release that cannot be signed)")
check("finalize-release" in needs_names("publish-update-manifest"),
      "publish-update-manifest waits for finalize-release "
      "(the latest slot never advertises an unfinalized release)")
check("sign-update-manifest" in needs_names("publish-update-manifest"),
      "publish-update-manifest consumes the signed manifest")
check("verify-published-packages" in needs_names("publish-update-manifest"),
      "publish-update-manifest depends on verify-published-packages")

# Alpine ships libcrypto but not the openssl CLI; both update jobs must install
# it or signing/verification silently has no tool.
for job in ["sign-update-manifest", "publish-update-manifest"]:
    before = " ".join(str(x) for x in resolve_extends(job).get("before_script", []))
    check("openssl" in before, f"{job} installs the openssl CLI")
    check(resolve_extends(job).get("image") == "alpine:3.22",
          f"{job} uses the pinned alpine image")
    check(resolve_extends(job).get("resource_group")
          == "lightning-project-6-publication",
          f"{job} shares the project 6 publication resource group")

# The signed manifest and its signature must actually be handed downstream.
sign_paths = doc["sign-update-manifest"]["artifacts"]["paths"]
check("dist/update-manifest-v1.json" in sign_paths
      and "dist/update-manifest-v1.json.sig" in sign_paths,
      "sign-update-manifest publishes the manifest and its signature as artifacts")

# --- GitHub bandwidth mirror (MIRROR-SPEC §6) --------------------------------
# GitLab stays the release authority; the mirror only holds byte-identical
# copies of what GitLab already published. Two ordering facts carry the whole
# design and are asserted in both directions:
#
#   AFTER finalize-release, because a GitHub release may only be created at a
#   tag the GitLab release already produced (and that the push mirror has
#   delivered).
#
#   BEFORE publish-update-manifest, because the `latest` slot is what every
#   installed Lightning polls. A mirror_url a client can read must already have
#   been proved to serve the right bytes.
check(doc["mirror-release-to-github"].get("stage") == "mirror",
      "mirror-release-to-github runs in the mirror stage")
check("finalize-release" in needs_names("mirror-release-to-github"),
      "the mirror runs after finalize-release "
      "(GitLab creates the release; GitHub only copies its bytes)")
check("mirror-release-to-github" in needs_names("publish-update-manifest"),
      "the latest manifest is promoted only after the mirror assets verify")
check("publish-packages" in needs_names("mirror-release-to-github"),
      "the mirror consumes the publication manifest")
check("verify-published-packages" in needs_names("mirror-release-to-github"),
      "the mirror runs only from a verified publication")
check("sign-update-manifest" in needs_names("mirror-release-to-github"),
      "the mirror cross-checks the signed manifest's mirror URLs")
# It uploads the published bytes, so it needs the jobs that carry them.
for producer in ["validate-deb", "validate-rpm", "validate-flatpak",
                 "validate-appimage", "validate-snap", "build-windows"]:
    check(producer in needs_names("mirror-release-to-github"),
          f"the mirror receives {producer}'s artifact bytes (it never rebuilds)")
# SHA256SUMS is generated inside publish-packages and is a published release
# file; without it in that job's artifacts the mirror cannot carry it.
check("dist/SHA256SUMS" in doc["publish-packages"]["artifacts"]["paths"],
      "publish-packages hands SHA256SUMS downstream for the mirror")
mirror_before = " ".join(
    str(x) for x in resolve_extends("mirror-release-to-github").get("before_script", []))
check("curl" in mirror_before and "jq" in mirror_before,
      "mirror-release-to-github installs curl and jq")
check(resolve_extends("mirror-release-to-github").get("image") == "alpine:3.22",
      "mirror-release-to-github uses the pinned alpine image")
check(resolve_extends("mirror-release-to-github").get("resource_group")
      == "lightning-project-6-publication",
      "mirror-release-to-github shares the publication resource group")

# No update-manifest or mirror script may be invoked from a job outside the
# publish gate.
UPDATE_SCRIPTS = ("generate-update-manifest.sh", "sign-update-manifest.sh",
                  "publish-update-manifest.sh", "mirror-release-to-github.sh")
GATED_UPDATE_JOBS = ("sign-update-manifest", "publish-update-manifest",
                     "mirror-release-to-github")
for job_name, job_def in doc.items():
    if not isinstance(job_def, dict):
        continue
    script_text = yaml.dump(job_def.get("script", []))
    if not any(s in script_text for s in UPDATE_SCRIPTS):
        continue
    check(job_name in GATED_UPDATE_JOBS,
          f"{job_name} is one of the gated update/mirror jobs")
    check(job_def.get("extends") == ".publish-rules",
          f"{job_name} extends the shared publish gate")

# The operator key-generation tool is never run by CI: it writes a private key.
for job_name, job_def in doc.items():
    if not isinstance(job_def, dict):
        continue
    check("generate-update-signing-key.sh" not in yaml.dump(job_def.get("script", [])),
          f"{job_name} does not run the operator key-generation tool")

# resolve-source runs validate-release-request.sh, which derives the public half
# of the update-signing key and compares it with the value every package embeds.
# Without the openssl CLI that gate cannot run at all, and the mismatch it
# exists to catch would only surface after every package had already been built
# around the wrong trust root.
resolve_before = " ".join(
    str(x) for x in resolve_extends("resolve-source").get("before_script", []))
check("openssl" in resolve_before,
      "resolve-source installs openssl for the update-signing consistency gate")

config_tests = doc["config-tests"]
config_script = yaml.dump(config_tests.get("script", []))
check("./tests/test-update-manifest.sh" in config_script,
      "config-tests runs the update-manifest suite")
check("openssl" in yaml.dump(config_tests.get("before_script", [])),
      "config-tests installs openssl for the update-manifest suite")
check("./tests/test-windows-version-resources.sh" in config_script,
      "config-tests runs the Windows version-resource suite")
config_before = yaml.dump(config_tests.get("before_script", []))
# A COMPLETE toolchain, not just a compiler driver. Pipelines 99 and 100 both
# died here: `gcc` alone under --no-install-recommends omits libc6-dev so the
# compiler cannot link, and without `make` CMake has no default generator.
# build-essential is the package that means all of it; accept an explicit
# equivalent set too, so this does not have to change again if the list is
# unpacked.
_has_toolchain = "build-essential" in config_before or (
    "gcc" in config_before
    and "libc6-dev" in config_before
    and "make" in config_before
)
check("cmake" in config_before and _has_toolchain,
      "config-tests installs cmake and a COMPLETE C toolchain for that suite")

# The snap repackages the AppImage job's bundled AppDir instead of
# recompiling Qt + Rust a third time.
check("build-appimage" in needs_names("build-snap"),
      "build-snap consumes the build-appimage AppDir artifact")


# --- minimal rule evaluator for the shared gate ---
def evaluate(rules, variables):
    """Return True if any rule matches (simple &&-of-equality expressions)."""
    for rule in rules:
        if "when" in rule and "if" not in rule:
            return rule["when"] != "never"
        expr = rule.get("if")
        if expr is None:
            return rule.get("when", "on_success") != "never"
        if eval_expr(expr, variables):
            return rule.get("when", "on_success") != "never"
    return False


def eval_expr(expr, v):
    def sub(tok):
        tok = tok.strip()
        if tok.startswith("$"):
            return v.get(tok[1:], "")
        return tok.strip('"')
    for clause in re.split(r"&&", expr):
        m = re.match(r"\s*(\S+)\s*=~\s*/(.+)/\s*$", clause)
        if m:
            if not re.search(m.group(2), sub(m.group(1))):
                return False
            continue
        m = re.match(r"\s*(\S+)\s*(==|!=)\s*(\S+)\s*", clause)
        if not m:
            return False
        left, op, right = sub(m.group(1)), m.group(2), sub(m.group(3))
        if op == "==" and left != right:
            return False
        if op == "!=" and left == right:
            return False
    return True


gate = resolve_extends("publish-packages")["rules"]
base = {"CI_COMMIT_BRANCH": "main", "CI_DEFAULT_BRANCH": "main"}
for action in ["create", "attach-existing"]:
    v = dict(base, PUBLISH_PACKAGES="true", RELEASE_ACTION=action)
    check(evaluate(gate, v), f"publish jobs run for RELEASE_ACTION={action}")
v = dict(base, PUBLISH_PACKAGES="false", RELEASE_ACTION="attach-existing")
check(not evaluate(gate, v), "publish jobs excluded for build-only (PUBLISH_PACKAGES=false)")

# A build-only pipeline uploads NOTHING: every publish/verify/release job (the
# only jobs that write to project 6's registry or touch a release) must be
# excluded when PUBLISH_PACKAGES=false, for both release actions and on any
# branch. Nothing else in the graph performs an upload.
for action in ["create", "attach-existing"]:
    for branch in ["main", "feature"]:
        v = dict(PUBLISH_PACKAGES="false", RELEASE_ACTION=action,
                 CI_COMMIT_BRANCH=branch, CI_DEFAULT_BRANCH="main")
        for job in publish_jobs:
            g = resolve_extends(job)["rules"]
            check(not evaluate(g, v),
                  f"build-only ({action}/{branch}): {job} excluded (no upload)")
v = dict(PUBLISH_PACKAGES="true", RELEASE_ACTION="create",
         CI_COMMIT_BRANCH="feature", CI_DEFAULT_BRANCH="main")
check(not evaluate(gate, v), "publish jobs excluded off the default branch")

# --- BUILD_FORMATS selection: build-only pipelines may run a subset, while
# --- publishing pipelines always include every format ---
def build_included(fmt, variables):
    return evaluate(doc["build-" + fmt]["rules"], variables)

all_fmts = list(FORMAT_SELECTOR)
v = {"PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "all"}
check(all(build_included(f, v) for f in all_fmts),
      "BUILD_FORMATS=all includes every build job")
v = {"PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "deb"}
check(build_included("deb", v) and not build_included("rpm", v)
      and not build_included("flatpak", v),
      "BUILD_FORMATS=deb builds only the deb")
v = {"PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "flatpak"}
check(build_included("flatpak", v) and not build_included("deb", v),
      "BUILD_FORMATS=flatpak builds only the flatpak")
v = {"PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "snap"}
check(not build_included("snap", v),
      "snap alone is excluded (needs the appimage AppDir)")
v = {"PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "appimage,snap"}
check(build_included("snap", v) and build_included("appimage", v),
      "snap runs when appimage is also selected")
v = {"PUBLISH_PACKAGES": "true", "BUILD_FORMATS": "deb"}
check(all(build_included(f, v) for f in all_fmts),
      "publishing pipelines build every format regardless of BUILD_FORMATS")
for fmt in all_fmts:
    check(doc["validate-" + fmt].get("rules") == doc["build-" + fmt].get("rules"),
          f"validate-{fmt} carries the same selection rules as its build")

# --- Windows is a separate, restrictive, non-publishing manual test path ---
windows = resolve_extends("windows-package-test")
check(set(windows.get("tags", [])) == {"windows-cross", "windows-package"},
      "Windows job uses only the dedicated cross-package runner tags")
WINDOWS_IMAGE = "lightning-windows-builder:fedora44-qt6.11.1-ffmpeg7.1.1-gst1.28.5-rust1.95.0-v3"
image = windows.get("image", {})
check(isinstance(image, dict)
      and image.get("name") == WINDOWS_IMAGE
      and image.get("pull_policy") == "if-not-present",
      "Windows job uses the pinned local FFmpeg-enabled builder image")
windows_rule_text = yaml.dump(windows.get("rules", []))
for required in ("CI_DEFAULT_BRANCH", "CI_PIPELINE_SOURCE", "BUILD_WINDOWS_PACKAGES",
                 "PUBLISH_PACKAGES", "BUILD_FORMATS", "SOURCE_REF",
                 "RELEASE_VERSION", "RELEASE_NOTES_B64"):
    check(required in windows_rule_text, f"Windows gate constrains {required}")
check("release:" not in yaml.dump(doc["windows-package-test"]),
      "Windows job has no GitLab release action")
check(windows.get("artifacts", {}).get("expire_in") == "7 days",
      "Windows test artifacts expire in seven days")
check(windows.get("artifacts", {}).get("access") == "developer",
      "Windows test artifacts are limited to developers")

windows_gate = windows["rules"]
windows_vars = {
    "CI_COMMIT_BRANCH": "main", "CI_DEFAULT_BRANCH": "main",
    "CI_PIPELINE_SOURCE": "web", "BUILD_WINDOWS_PACKAGES": "true",
    "PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "none",
    "SOURCE_REF": "5" * 40, "RELEASE_VERSION": "", "RELEASE_NOTES_B64": "",
}
check(evaluate(windows_gate, windows_vars),
      "trusted default-branch Windows-only request includes the Windows job")
for key, value in (("CI_COMMIT_BRANCH", "feature"),
                   ("CI_PIPELINE_SOURCE", "merge_request_event"),
                   ("BUILD_WINDOWS_PACKAGES", "false"),
                   ("PUBLISH_PACKAGES", "true"),
                   ("BUILD_FORMATS", "all"),
                   ("SOURCE_REF", "main"),
                   ("RELEASE_VERSION", "0.6.2"),
                   ("RELEASE_NOTES_B64", "bm90ZXM=")):
    rejected = dict(windows_vars)
    rejected[key] = value
    check(not evaluate(windows_gate, rejected),
          f"Windows gate rejects unsafe {key}={value}")
check(not any(build_included(fmt, {"PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "none"})
              for fmt in all_fmts),
      "BUILD_FORMATS=none excludes every Linux package build")

# --- build-windows: the publishing Windows job (0.6.3+). Same FFmpeg image and
#     runner as the test job, but publish-gated and feeding publish-packages so
#     the portable/MSI/setup artifacts publish from the same resolved commit. ---
build_windows = resolve_extends("build-windows")
bw_image = build_windows.get("image", {})
check(isinstance(bw_image, dict) and bw_image.get("name") == WINDOWS_IMAGE,
      "build-windows uses the FFmpeg-enabled builder image")
check(set(build_windows.get("tags", [])) == {"windows-cross", "windows-package"},
      "build-windows uses the dedicated cross-package runner tags")
bw_gate = build_windows["rules"]
check(evaluate(bw_gate, {"CI_COMMIT_BRANCH": "main", "CI_DEFAULT_BRANCH": "main",
                         "PUBLISH_PACKAGES": "true"}),
      "build-windows runs in a publishing pipeline on the default branch")
check(not evaluate(bw_gate, {"CI_COMMIT_BRANCH": "main", "CI_DEFAULT_BRANCH": "main",
                             "PUBLISH_PACKAGES": "false"}),
      "build-windows is excluded when not publishing")
check(not evaluate(bw_gate, {"CI_COMMIT_BRANCH": "feature", "CI_DEFAULT_BRANCH": "main",
                             "PUBLISH_PACKAGES": "true"}),
      "build-windows is excluded off the default branch")

# --- macOS: native shell-executor test path on the physical Mac mini ---------
# Same restrictive shape as the Windows gate, with two extra guarantees: it is
# mutually exclusive with the Windows job, and it has NO publishing counterpart
# anywhere in the graph (unsigned/un-notarized bundles must never be published).
macos = resolve_extends("macos-package-test")
check(set(macos.get("tags", [])) == {"macos", "arm64"},
      "macOS job selects only the Apple Silicon runner tags")
# A shell-executor job must not carry an image: there is no macOS container.
check("image" not in macos,
      "macOS job declares no image (shell executor on bare metal)")
check(macos.get("stage") == "build", "macOS job runs in the build stage")
check("resolve-source" in needs_names("macos-package-test"),
      "macOS job consumes the resolve-source artifacts")

# width is not cosmetic here: the gate is one long `if:` expression and yaml's
# default 80-column wrapping splits it mid-token, which silently breaks every
# substring assertion below.
macos_rule_text = yaml.dump(macos.get("rules", []), width=10**6)
for required in ("CI_DEFAULT_BRANCH", "CI_PIPELINE_SOURCE", "BUILD_MACOS_PACKAGES",
                 "SOURCE_REF"):
    check(required in macos_rule_text, f"macOS gate constrains {required}")
check("release:" not in yaml.dump(doc["macos-package-test"]),
      "macOS job has no GitLab release action")
check(macos.get("artifacts", {}).get("expire_in") == "7 days",
      "macOS test artifacts expire in seven days")
check(macos.get("artifacts", {}).get("access") == "developer",
      "macOS test artifacts are limited to developers")

macos_gate = macos["rules"]
macos_vars = {
    "CI_COMMIT_BRANCH": "main", "CI_DEFAULT_BRANCH": "main",
    "CI_PIPELINE_SOURCE": "web", "BUILD_MACOS_PACKAGES": "true",
    "BUILD_WINDOWS_PACKAGES": "false",
    "PUBLISH_PACKAGES": "false", "BUILD_FORMATS": "none",
    "SOURCE_REF": "7" * 40, "RELEASE_VERSION": "", "RELEASE_NOTES_B64": "",
}
check(evaluate(macos_gate, macos_vars),
      "trusted default-branch macOS-only request includes the macOS job")
for key, value in (("CI_COMMIT_BRANCH", "feature"),
                   ("CI_PIPELINE_SOURCE", "merge_request_event"),
                   ("BUILD_MACOS_PACKAGES", "false"),
                   ("SOURCE_REF", "main")):
    rejected = dict(macos_vars)
    rejected[key] = value
    check(not evaluate(macos_gate, rejected),
          f"macOS gate rejects unsafe {key}={value}")

# A macOS-only request (BUILD_FORMATS=none) creates no Linux or Windows build.
check(not any(build_included(fmt, macos_vars) for fmt in all_fmts),
      "a macOS-only request creates no Linux package build")
check(not evaluate(resolve_extends("windows-package-test")["rules"], macos_vars),
      "a macOS-only request creates no Windows test job")
check(not evaluate(resolve_extends("build-windows")["rules"], macos_vars),
      "a macOS-only request creates no Windows publishing job")

# ...and the converse: the Mac is a dedicated host sharing nothing with the
# Linux/Windows pools, so a full-fleet or publishing run must NOT silently drop
# the macOS build. Excluding it would mean macOS missing from exactly the
# pipelines that build every other platform.
fleet_vars = dict(macos_vars, BUILD_FORMATS="all")
check(evaluate(macos_gate, fleet_vars),
      "macOS runs alongside a full-fleet build (BUILD_FORMATS=all)")
check(all(build_included(f, fleet_vars) for f in all_fmts),
      "the full-fleet request still builds every Linux format")
publish_fleet = dict(macos_vars, BUILD_FORMATS="all", PUBLISH_PACKAGES="true",
                     RELEASE_VERSION="0.6.6")
check(evaluate(macos_gate, publish_fleet),
      "macOS also runs in a publishing fleet pipeline")

# "Instantly" is a scheduling property, not a wish: the job must depend only on
# resolve-source (never on a Linux build) and must sit in its own resource
# group, or it would queue behind the two bounded Linux build lanes.
check(needs_names("macos-package-test") == ["resolve-source"],
      "macOS job waits only on resolve-source, never on a Linux build")
macos_group = macos.get("resource_group")
check(macos_group not in set(BUILD_GROUP.values()) and macos_group is not None,
      "macOS job has its own resource group, so it never queues behind Linux lanes")

# macOS reaches the publication chain as of 0.7.5, on a maintainer decision,
# and the guards changed shape rather than disappearing.
#
# What was asserted before — that nothing consumes or releases the bundle —
# existed because publishing something Gatekeeper blocks would hand users a
# file they cannot open. That is still true of the artifact; what changed is
# that the download page now tells them how to open it, and states the two
# limits plainly (Apple Silicon only, macOS 26 or newer). So the invariants
# that matter now are different ones: the release must not DEPEND on the Mac,
# and the bundle must not enter the signed update manifest.
check("macos-package-test" in needs_names("publish-packages"),
      "publish-packages consumes the macOS bundle")
# ...and so must the mirror, which uploads the PUBLISHED BYTES and refuses to
# rebuild them. Anything publish-packages puts into the publication manifest,
# mirror-release-to-github has to have on disk. Pipeline 110 proved the cost of
# getting this wrong: the packages published, the tag and the GitLab release
# were created, and only then did the mirror die on "mirror input missing" —
# the most expensive point in the run to discover a missing `needs`.
#
# Stated generally rather than as a second macOS line, because the next format
# added will have exactly the same requirement.
_publish_inputs = set(needs_names("publish-packages")) - {"resolve-source"}
_mirror_inputs = set(needs_names("mirror-release-to-github"))
check(_publish_inputs <= _mirror_inputs,
      "the mirror consumes every artifact source publish-packages does")
_missing = sorted(_publish_inputs - _mirror_inputs)
if _missing:
    print("     missing from mirror-release-to-github: %s" % ", ".join(_missing))
macos_need = next(n for n in doc["publish-packages"]["needs"]
                  if isinstance(n, dict) and n["job"] == "macos-package-test")
check(macos_need.get("optional") is True,
      "the macOS need is optional, so a pipeline without it still publishes")
check(macos.get("allow_failure") is True,
      "the macOS job is allow_failure, so a Mac outage cannot block a release")
check(not any(job.startswith("build-macos") for job in doc),
      "no second macOS build job exists")

# The bundle is a DOWNLOAD, never an update. The client has no macOS install
# strategy — InstallType::MacosDmg is not self-installable and the updater
# helper returns UnsupportedPlatform — so an entry in the signed update
# manifest would advertise an install the updater refuses to perform.
with open(os.path.join(HERE, "..", "scripts", "generate-update-manifest.sh"),
          encoding="utf-8") as handle:
    update_manifest_src = handle.read()
_formats_decl = re.search(r"update_formats=\(([^)]*)\)", update_manifest_src)
check(_formats_decl is not None, "the update manifest declares its formats")
check(_formats_decl is None or "macos" not in _formats_decl.group(1).lower(),
      "the update manifest carries no macOS artifact")

for job_name, job_def in doc.items():
    if not isinstance(job_def, dict) or "macos" not in str(job_def.get("tags", [])):
        continue
    check("release" not in job_def,
          f"{job_name} (macOS runner) has no release action")

# ---------------------------------------------------------------------------
# Voice/video calling runtime dependencies (2026-08-23).
#
# GStreamer PLUGINS are dlopen'd from a plugin path, so NOTHING that inspects
# ELF NEEDED entries can find them: dpkg-shlibdeps, rpm's automatic generator
# and linuxdeploy all miss them, because the binary links only gstreamer
# core/webrtc/sdp. Every packaging format therefore has to name them
# explicitly, and each one fails the same way if it stops: the package
# installs and launches perfectly, then refuses every call, because the
# engine's runtime element probe finds nothing. That is the worst kind of
# packaging regression — nothing about the symptom points at packaging.
_PLUGIN_SUBSTRINGS = ("plugins-base", "plugins-good", "plugins-bad")

with open(os.path.join(HERE, "..", "scripts", "build-deb.sh"),
          encoding="utf-8") as handle:
    deb_src = handle.read()
_deb_call = re.search(r'CALL_DEPENDS="([^"]*)"', deb_src)
check(_deb_call is not None, "build-deb declares CALL_DEPENDS")
if _deb_call:
    _deb_deps = _deb_call.group(1)
    for needle in _PLUGIN_SUBSTRINGS + ("nice", "pipewire"):
        check(needle in _deb_deps, f"deb depends on gstreamer {needle}")
check("$CALL_DEPENDS" in deb_src or "CALL_DEPENDS\"" in deb_src,
      "build-deb actually writes CALL_DEPENDS into the control file")

with open(os.path.join(HERE, "..", "packaging", "rpm", "lightning.spec"),
          encoding="utf-8") as handle:
    spec_src = handle.read()
_rpm_requires = "\n".join(
    line for line in spec_src.splitlines() if line.startswith("Requires:"))
for needle in _PLUGIN_SUBSTRINGS + ("libnice", "pipewire"):
    check(needle in _rpm_requires, f"rpm requires gstreamer {needle}")

with open(os.path.join(HERE, "..", "scripts", "build-appimage.sh"),
          encoding="utf-8") as handle:
    appimage_src = handle.read()
# The AppImage bundles rather than depends, so it needs BOTH halves: the
# plugins staged into the AppDir, and an AppRun hook pointing GStreamer at
# them. Staging without the hook bundles files nothing ever loads.
check("gstreamer-1.0" in appimage_src,
      "the AppImage stages GStreamer plugins into the AppDir")
check("apprun-hooks" in appimage_src,
      "the AppImage installs an AppRun hook for the bundled plugins")
check("GST_PLUGIN_SYSTEM_PATH_1_0" in appimage_src,
      "the AppRun hook points GStreamer at the bundled plugin path")

with open(os.path.join(HERE, "..", "packaging", "flatpak",
                       "org.lightning_matrix.Lightning.yaml.in"),
          encoding="utf-8") as handle:
    flatpak_src = handle.read()
# Screen capture is negotiated through xdg-desktop-portal (reachable from a
# sandbox by default), but the resulting stream is READ over the PipeWire
# socket, which is not.
check("xdg-run/pipewire-0" in flatpak_src,
      "the Flatpak can reach the PipeWire socket for portal streams")
# The portal decides what may be captured. Granting the host filesystem to
# avoid that dialog would defeat the sandbox for no benefit.
#
# Matched against ACTUAL finish-args entries, not the raw file: a substring
# search also hits the comment that explains why we do not use it, which is
# the "ban regex matching a token named in a comment" trap this repo has
# already been bitten by.
_flatpak_args = [
    line.strip()[2:].strip()          # drop the YAML "- " list marker only
    for line in flatpak_src.splitlines()
    if line.strip().startswith("- --")
]
check(not any(arg.startswith("--filesystem=host") for arg in _flatpak_args),
      "the Flatpak does not fall back to --filesystem=host")
check("--filesystem=xdg-run/pipewire-0" in _flatpak_args,
      "the PipeWire socket is an actual finish-arg, not just a comment")

# Windows bundles rather than depends, like the AppImage, and needs the same two
# halves plus a third the Linux formats get for free: the builder image must
# CARRY GStreamer at all, or CMake silently configures the engine out and every
# check downstream still passes.
with open(os.path.join(HERE, "..", "packaging", "windows", "Dockerfile"),
          encoding="utf-8") as handle:
    win_dockerfile = handle.read()
check("GSTREAMER_SHA256" in win_dockerfile and "gstreamer-1.0-mingw-x86_64" in win_dockerfile,
      "the Windows builder installs a checksum-pinned GStreamer MinGW SDK")
check("gstreamer-webrtc-1.0" in win_dockerfile,
      "the Windows builder verifies the WebRTC pkg-config module resolves")
# The two plugins whose libstdc++ imports the staged libstdc++-6.dll does not
# export (UCRT vs msvcrt `mbstate_t`). Naming them here keeps a future edit from
# re-adding them by reflex -- Wine loads them, so the element probe would not
# catch it, and the failure would land on a user's machine.
#
# Matched against the Dockerfile with its COMMENT LINES REMOVED: both names are
# in the comment that explains why they are excluded, so a raw substring search
# would report them present and the ban would be inverted.
_win_dockerfile_code = "\n".join(
    line for line in win_dockerfile.splitlines()
    if not line.lstrip().startswith("#"))
check("libgstwebrtc.dll" in _win_dockerfile_code,
      "the Dockerfile comment stripper still leaves the plugin install list")
for banned in ("libgstmediafoundation.dll", "libgstd3d11.dll"):
    check(banned not in _win_dockerfile_code,
          f"the Windows builder does not install {banned}")

with open(os.path.join(HERE, "..", "scripts", "stage-windows-runtime.py"),
          encoding="utf-8") as handle:
    win_stage_src = handle.read()
check('GSTREAMER_PLUGIN_DIR = "gstreamer-1.0"' in win_stage_src,
      "the Windows stage uses the gstreamer-1.0 directory the application reads")
for needle in ("libgstwebrtc.dll", "libgstnice.dll", "libgstdtls.dll",
               "libgstsrtp.dll", "libgstvpx.dll", "libgstopus.dll",
               "libgstwinks.dll", "libgstwinscreencap.dll"):
    check(needle in win_stage_src, f"the Windows stage bundles {needle}")

with open(os.path.join(HERE, "..", "scripts", "validate-windows-artifacts.sh"),
          encoding="utf-8") as handle:
    win_validate_src = handle.read()
# The ONE fact no file listing can show: an engine-less build is a normal,
# launchable, syncing package.
check("libgstwebrtc-1.0-0.dll" in win_validate_src,
      "Windows validation proves the application links the call media engine")
check("gst-element-probe.exe" in win_validate_src,
      "Windows validation runs the packaged tree's own element probe")

if errors:
    print(f"\nPipeline config tests FAILED ({len(errors)})", file=sys.stderr)
    sys.exit(1)
print("Pipeline config tests passed")
