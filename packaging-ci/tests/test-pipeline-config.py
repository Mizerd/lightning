#!/usr/bin/env python3
"""Structural and rule tests for .gitlab-ci.yml.

Proves the pipeline contains the required six-stage graph, that the publish,
verify, and release jobs are gated only on PUBLISH_PACKAGES (so both release
actions include them), that they are correctly wired downstream of validation
and each other, and that a build-only pipeline excludes them entirely.
"""
import ast
import os
import re
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
# GitLab reads .gitlab-ci.yml only from the repository root, above this tree.
CI = os.path.join(HERE, "..", "..", ".gitlab-ci.yml")

errors = []


def check(cond, msg):
    if cond:
        print(f"  ok: {msg}")
    else:
        # Flush stdout first: when piped it is block-buffered, so under
        # `2>&1` a FAIL could land mid-line and escape `grep -c '^ *FAIL'`.
        sys.stdout.flush()
        print(f"  FAIL: {msg}", file=sys.stderr)
        sys.stderr.flush()
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
    "build-deb-ubuntu", "validate-deb-ubuntu",
    "validate-deb", "validate-rpm",
    "validate-flatpak", "validate-appimage", "validate-snap",
    "publish-packages", "verify-published-packages", "finalize-release",
    "sign-update-manifest", "mirror-release-to-github", "publish-update-manifest",
    "github-mirror-preflight", "mirror-update-manifest-to-github",
    "windows-package-test", "build-windows", "macos-package-test",
]
for job in required_jobs:
    check(job in doc, f"job {job} is defined")

# --- every validated package must be consumed by publish-packages; an
# --- unconsumed lane is invisible in a green pipeline.
publish_needs = {
    n["job"] if isinstance(n, dict) else n
    for n in doc.get("publish-packages", {}).get("needs", [])
}
# Derived from the file so a new format's validator cannot be missed.
validators = sorted(
    j for j in doc
    if isinstance(j, str) and j.startswith("validate-")
    and isinstance(doc.get(j), dict)
)
check(len(validators) >= 6, f"found the validate-* jobs ({len(validators)})")
for job in validators:
    check(job in publish_needs, f"publish-packages consumes {job}")

# --- one dedicated runner pool per format: every build/validate job selects
# --- exactly one unique selector tag. Each selector matches a runner on both
# --- the GitLab VM fleet and the mirrored fleet on 10.195.35.6, so whichever
# --- matching runner is free takes the job.
UNIQUE_SELECTORS = {"apt", "dnf", "nix", "flatpak", "appimage", "snap"}
FORMAT_SELECTOR = {
    "deb": "apt", "rpm": "dnf",
    "flatpak": "flatpak", "appimage": "appimage", "snap": "snap",
}
# Cross-check the hard-coded list against the file: several checks below
# iterate FORMAT_SELECTOR, so a new format missing here would skip them.
# validate-deb-ubuntu reuses validate-deb.sh.
_UBUNTU_REUSES = {"deb-ubuntu": "deb"}
_derived_formats = {
    _UBUNTU_REUSES.get(j[len("validate-"):], j[len("validate-"):])
    for j in validators
}
check(_derived_formats == set(FORMAT_SELECTOR),
      "FORMAT_SELECTOR names exactly the formats .gitlab-ci.yml validates "
      f"(file: {sorted(_derived_formats)}, list: {sorted(FORMAT_SELECTOR)})")
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
# Each runner host runs one package job at a time (concurrent=1), so the two
# resource groups are what spreads parallel lanes across both hosts; both must
# be non-empty.
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
# Signing runs before the release, so a broken key fails before anything is
# irreversible. Publishing `latest` runs after it, so clients are never told
# about a release that does not exist.
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

# Alpine ships libcrypto but not the openssl CLI.
for job in ["sign-update-manifest", "publish-update-manifest"]:
    before = " ".join(str(x) for x in resolve_extends(job).get("before_script", []))
    check("openssl" in before, f"{job} installs the openssl CLI")
    check(str(resolve_extends(job).get("image", "")).startswith("alpine@sha256:"),
          f"{job} uses the digest-pinned alpine image")
    check(resolve_extends(job).get("resource_group")
          == "lightning-project-6-publication",
          f"{job} shares the project 6 publication resource group")

# The signed manifest and its signature must actually be handed downstream.
sign_paths = doc["sign-update-manifest"]["artifacts"]["paths"]
check("dist/update-manifest-v1.json" in sign_paths
      and "dist/update-manifest-v1.json.sig" in sign_paths,
      "sign-update-manifest publishes the manifest and its signature as artifacts")

# --- GitHub bandwidth mirror (MIRROR-SPEC §6) --------------------------------
# The mirror runs after finalize-release (the tag must exist) and before
# publish-update-manifest (every mirror_url must serve the right bytes before
# clients can read it).
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
# SHA256SUMS is generated in publish-packages; the mirror needs it too.
check("dist/SHA256SUMS" in doc["publish-packages"]["artifacts"]["paths"],
      "publish-packages hands SHA256SUMS downstream for the mirror")
# The mirror release carries the release notes, from the same
# dist/release-notes.md that finalize-release uses, before the mirror notice.
check("resolve-source" in needs_names("mirror-release-to-github"),
      "the mirror receives resolve-source's artifacts (release-notes.md lives there)")
check("dist/release-notes.md" in doc["resolve-source"]["artifacts"]["paths"],
      "resolve-source hands the release notes downstream")
with open(os.path.join(HERE, os.pardir, "scripts",
                       "mirror-release-to-github.sh"), encoding="utf-8") as fh:
    _mirror_sh = fh.read()
check("dist/release-notes.md" in _mirror_sh,
      "the mirror release body includes the release notes")
check(_mirror_sh.index("dist/release-notes.md")
      < _mirror_sh.index("This is a read-only mirror"),
      "the release notes come before the mirror notice in the body")

mirror_before = " ".join(
    str(x) for x in resolve_extends("mirror-release-to-github").get("before_script", []))
check("curl" in mirror_before and "jq" in mirror_before,
      "mirror-release-to-github installs curl and jq")
check(str(resolve_extends("mirror-release-to-github").get("image", "")).startswith("alpine@sha256:"),
      "mirror-release-to-github uses the digest-pinned alpine image")
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

# The update-signing key gate in resolve-source needs the openssl CLI.
resolve_before = " ".join(
    str(x) for x in resolve_extends("resolve-source").get("before_script", []))
check("openssl" in resolve_before,
      "resolve-source installs openssl for the update-signing consistency gate")

config_tests = doc["config-tests"]
config_script = yaml.dump(config_tests.get("script", []))
check("./packaging-ci/tests/test-update-manifest.sh" in config_script,
      "config-tests runs the update-manifest suite")
check("openssl" in yaml.dump(config_tests.get("before_script", [])),
      "config-tests installs openssl for the update-manifest suite")
check("./packaging-ci/tests/test-windows-version-resources.sh" in config_script,
      "config-tests runs the Windows version-resource suite")
# Every test file in packaging-ci/tests must actually be run by the job.
_tests_dir = os.path.join(HERE)
for _entry in sorted(os.listdir(_tests_dir)):
    if not _entry.startswith("test-"):
        continue
    if not _entry.endswith((".py", ".sh")):
        continue
    check(_entry in config_script,
          f"config-tests actually runs {_entry}")
config_before = yaml.dump(config_tests.get("before_script", []))
# A complete toolchain: under --no-install-recommends `gcc` lacks libc6-dev,
# and CMake needs `make`. Accept build-essential or the explicit set.
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

# A build-only pipeline uploads nothing: every publishing job is excluded for
# both release actions and on any branch.
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
WINDOWS_IMAGE = "lightning-windows-builder:fedora44-qt6.11.2-ffmpeg7.1.1-gst1.28.5-rust1.95.0-v7"
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

# --- build-windows: the publishing Windows job. Same image and runner as the
#     test job, but publish-gated and feeding publish-packages. ---
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
# Same restrictive gate shape as the Windows test job.
macos = resolve_extends("macos-package-test")
check(set(macos.get("tags", [])) == {"macos", "arm64"},
      "macOS job selects only the Apple Silicon runner tags")
# Shell executor: there is no macOS container image.
check("image" not in macos,
      "macOS job declares no image (shell executor on bare metal)")
check(macos.get("stage") == "build", "macOS job runs in the build stage")
check("resolve-source" in needs_names("macos-package-test"),
      "macOS job consumes the resolve-source artifacts")

# Unlimited width: default wrapping would split the long `if:` mid-token and
# break the substring checks below.
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

# ---- the missing-asset report ------------------------------------------------
#
# macOS is optional for publication, so report-optional-assets reports a
# missing macOS asset after the release without being able to block it.
report = resolve_extends("report-optional-assets")
check(report.get("stage") == "update",
      "the optional-asset report runs in the LAST stage")
check(report.get("allow_failure", False) is False,
      "the optional-asset report is NOT allow_failure -- that is its whole point")
check("finalize-release" in needs_names("report-optional-assets"),
      "the optional-asset report runs after the release is finalized")
# Nothing may depend on it, or a red report would block publication.
dependents = [j for j in doc
              if isinstance(doc[j], dict)
              and "report-optional-assets" in needs_names(j)]
check(not dependents,
      f"nothing needs the optional-asset report (found {dependents})")
check(evaluate(report["rules"],
               dict(macos_vars, PUBLISH_PACKAGES="true", BUILD_FORMATS="all")),
      "the optional-asset report runs on a publishing pipeline")
check(not evaluate(report["rules"], macos_vars),
      "the optional-asset report does not run on a non-publishing pipeline")

# A macOS-only request (BUILD_FORMATS=none) creates no Linux or Windows build.
check(not any(build_included(fmt, macos_vars) for fmt in all_fmts),
      "a macOS-only request creates no Linux package build")
check(not evaluate(resolve_extends("windows-package-test")["rules"], macos_vars),
      "a macOS-only request creates no Windows test job")
check(not evaluate(resolve_extends("build-windows")["rules"], macos_vars),
      "a macOS-only request creates no Windows publishing job")

# Conversely, a full-fleet or publishing run must include the macOS build;
# the Mac shares no capacity with the Linux/Windows pools.
fleet_vars = dict(macos_vars, BUILD_FORMATS="all")
check(evaluate(macos_gate, fleet_vars),
      "macOS runs alongside a full-fleet build (BUILD_FORMATS=all)")
check(all(build_included(f, fleet_vars) for f in all_fmts),
      "the full-fleet request still builds every Linux format")
publish_fleet = dict(macos_vars, BUILD_FORMATS="all", PUBLISH_PACKAGES="true",
                     RELEASE_VERSION="0.6.6")
check(evaluate(macos_gate, publish_fleet),
      "macOS also runs in a publishing fleet pipeline")

# The macOS job must start immediately: depend only on resolve-source and use
# its own resource group, never queueing behind the Linux build lanes.
check(needs_names("macos-package-test") == ["resolve-source"],
      "macOS job waits only on resolve-source, never on a Linux build")
macos_group = macos.get("resource_group")
check(macos_group not in set(BUILD_GROUP.values()) and macos_group is not None,
      "macOS job has its own resource group, so it never queues behind Linux lanes")

# macOS is published as a download-only asset. The invariants: the release
# must not depend on the Mac, and the bundle must not enter the signed update
# manifest.
check("macos-package-test" in needs_names("publish-packages"),
      "publish-packages consumes the macOS bundle")
# The mirror uploads the published bytes without rebuilding, so it needs every
# artifact source publish-packages consumes; a missing `needs` fails only after
# the release exists. Gates needed with `artifacts: false` are excluded.
def _artifact_needs(job):
    out = set()
    for n in doc[job].get("needs", []):
        if isinstance(n, dict):
            if n.get("artifacts", True):
                out.add(n["job"])
        else:
            out.add(n)
    return out
_publish_inputs = _artifact_needs("publish-packages") - {"resolve-source"}
_mirror_inputs = _artifact_needs("mirror-release-to-github")
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

# The bundle is a download, never an update: the client cannot self-install
# on macOS (InstallType::MacosDmg, UnsupportedPlatform).
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
# Voice/video calling runtime dependencies.
#
# GStreamer plugins are dlopen'd, so dpkg-shlibdeps, rpm's dependency generator
# and linuxdeploy cannot see them. Every format must name them explicitly, or
# the package installs and runs but refuses every call.
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
# The AppImage needs both the staged plugins and an AppRun hook pointing
# GStreamer at them.
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
# The portal negotiates screen capture, but the stream is read over the
# PipeWire socket, which the sandbox does not expose by default.
check("xdg-run/pipewire-0" in flatpak_src,
      "the Flatpak can reach the PipeWire socket for portal streams")
# No --filesystem=host: the portal decides what may be captured. Matched
# against actual finish-args, since a comment may name the banned flag.
_flatpak_args = [
    line.strip()[2:].strip()          # drop the YAML "- " list marker only
    for line in flatpak_src.splitlines()
    if line.strip().startswith("- --")
]
check(not any(arg.startswith("--filesystem=host") for arg in _flatpak_args),
      "the Flatpak does not fall back to --filesystem=host")
check("--filesystem=xdg-run/pipewire-0" in _flatpak_args,
      "the PipeWire socket is an actual finish-arg, not just a comment")

# Windows bundles like the AppImage, and the builder image must also carry
# GStreamer, or CMake silently configures the engine out.
with open(os.path.join(HERE, "..", "packaging", "windows", "Dockerfile"),
          encoding="utf-8") as handle:
    win_dockerfile = handle.read()
check("GSTREAMER_SHA256" in win_dockerfile and "gstreamer-1.0-mingw-x86_64" in win_dockerfile,
      "the Windows builder installs a checksum-pinned GStreamer MinGW SDK")
check("gstreamer-webrtc-1.0" in win_dockerfile,
      "the Windows builder verifies the WebRTC pkg-config module resolves")
# These two plugins import libstdc++ symbols the staged libstdc++-6.dll lacks
# (UCRT vs msvcrt `mbstate_t`); Wine loads them anyway, so only this ban
# catches them. Comment lines are stripped because they name both DLLs.
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
               "libgstwinks.dll", "libgstwinscreencap.dll",
               # webrtcbin loads sctp itself for the data channel that owns
               # the bundled transport of LiveKit's subscriber offer; without
               # it Windows can send but receives nothing.
               "libgstsctp.dll"):
    check(needle in win_stage_src, f"the Windows stage bundles {needle}")
# Probe the elements too: libgstsctp-1.0-0.dll is the SCTP library, not the
# plugin.
for element in ("sctpenc", "sctpdec"):
    check(f'"{element}"' in win_stage_src,
          f"the Windows element probe covers {element}")
# jpegenc is required although no media pipeline uses it:
# SfuMediaEngine::jpegCameraChainAvailable() builds a test chain with it, and
# without it every camera silently falls back to the slower raw chain.
# The element list is parsed, not grepped, so a name left only in a comment
# does not count, and its exact size is asserted.
_win_elements = None
for _node in ast.walk(ast.parse(win_stage_src)):
    if isinstance(_node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == "GSTREAMER_ELEMENTS"
            for t in _node.targets):
        _win_elements = [
            e.value for e in _node.value.elts
            if isinstance(e, ast.Constant) and isinstance(e.value, str)
        ]
check(_win_elements is not None,
      "GSTREAMER_ELEMENTS is a parseable literal in stage-windows-runtime.py")
check(len(_win_elements or []) == 43,
      f"the Windows element probe asks for 43 elements (found "
      f"{len(_win_elements or [])})")
for element in ("jpegdec", "jpegenc", "level", "sctpenc", "sctpdec"):
    check(element in (_win_elements or []),
          f"the Windows element probe covers {element}")

# ---------------------------------------------------------------------------
# The call media engine must be built into every Linux package.
#
# LIGHTNING_ENABLE_WEBRTC is honoured only when pkg-config finds the GStreamer
# WebRTC development files; otherwise CMake quietly configures the engine out.
# So:
#   1. every Linux job that compiles installs the development files;
#   2. the build asserts the resulting binary carries the engine;
#   3. every per-format validator asks the shipped artifact whether calling
#      works.


def _strip_shell_comments(text):
    """Drop whole-line shell comments.

    Every explanation in these scripts names the very strings asserted below,
    so a raw substring search would pass on a tree with the code removed. Only
    FULL-LINE comments are dropped, which is where the prose lives and which
    cannot be confused with a `#` inside a string.
    """
    return "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#"))


def _read(*parts):
    # .gitlab-ci.yml is at the repository root; the rest is in packaging-ci.
    base = (HERE, "..", "..") if parts and parts[0] == ".gitlab-ci.yml" \
        else (HERE, "..")
    with open(os.path.join(*base, *parts), encoding="utf-8") as handle:
        return handle.read()


# --- 1. the development files, in every job that compiles -------------------
#
# Not build-flatpak (the org.kde.Sdk supplies the modules) or build-snap (it
# repacks the AppImage's AppDir).
_GST_DEV_PACKAGES = {
    # Debian/Ubuntu: together these provide all six pkg_check_modules modules.
    "build-deb": ("libgstreamer1.0-dev", "libgstreamer-plugins-base1.0-dev",
                  "libgstreamer-plugins-bad1.0-dev"),
    "build-appimage": ("libgstreamer1.0-dev", "libgstreamer-plugins-base1.0-dev",
                       "libgstreamer-plugins-bad1.0-dev"),
    # Fedora, same split.
    "build-rpm": ("gstreamer1-devel", "gstreamer1-plugins-base-devel",
                  "gstreamer1-plugins-bad-free-devel"),
}
for job, packages in _GST_DEV_PACKAGES.items():
    # The parsed YAML carries only commands, not comments.
    script_text = " ".join(resolve_extends(job).get("before_script", []))
    for package in packages:
        check(package in script_text,
              f"{job} installs the GStreamer dev package {package}")

# build-snap.sh stages /usr/share/X11/xkb and /etc/fonts, which core24 lacks
# and without which the confined snap crashes on start. The job image has
# neither unless these packages are installed explicitly.
_snap_before = " ".join(resolve_extends("build-snap").get("before_script", []))
for package in ("xkb-data", "fontconfig-config"):
    check(package in _snap_before,
          f"build-snap installs {package}, without which the payload stages "
          f"no {'keymaps' if package == 'xkb-data' else 'fontconfig'} and the "
          f"snap cannot start")

# The AppImage (and the snap built from it) bundles the runtime plugins.
_appimage_before = " ".join(resolve_extends("build-appimage").get("before_script", []))
for package in ("gstreamer1.0-plugins-base", "gstreamer1.0-plugins-good",
                "gstreamer1.0-plugins-bad", "gstreamer1.0-nice",
                "gstreamer1.0-pipewire", "gstreamer1.0-alsa"):
    check(package in _appimage_before,
          f"build-appimage installs the runtime plugin package {package}")

# libnss3 is pinned by name although it currently arrives transitively.
# libsrtp2 uses NSS, which dlopens libsoftokn3/libfreebl3 at runtime; without
# them staged, calls carry no media in either direction.
check("libnss3" in _appimage_before,
      "build-appimage installs libnss3, whose nss/ modules build-appimage.sh "
      "stages beside libnss3.so -- without them libsrtp cannot initialise and "
      "every call is silent in both directions")

# `appstreamcli compose` rasterises the scalable app-id icon and needs
# gdk-pixbuf's SVG loader module (librsvg2-common), which
# --no-install-recommends omits. Without it the build fails with
# `file-read-error` / `filters-but-no-output`, hints that name no file.
_flatpak_before = " ".join(resolve_extends("build-flatpak").get("before_script", []))
check("librsvg2-common" in _flatpak_before,
      "build-flatpak installs librsvg2-common, the gdk-pixbuf SVG loader "
      "module appstreamcli compose needs to read the scalable icon")

# build-flatpak.sh also checks this up front, before compiling.
_flatpak_src = _read("scripts", "build-flatpak.sh")
# Count calls, not occurrences: the definition alone would satisfy a
# substring test.
_preflight_calls = [
    line for line in _flatpak_src.splitlines()
    if "appstream_can_read_scalable_icon" in line
    and "() {" not in line
    and not line.lstrip().startswith("#")
]
check(_preflight_calls,
      "build-flatpak.sh CALLS its scalable-icon preflight, not merely "
      "defines it")

# --- 2. the build refuses to produce an engine-less binary ------------------
configure_src = _strip_shell_comments(_read("scripts", "configure-build.sh"))
check("-DLIGHTNING_ENABLE_WEBRTC=ON" in configure_src,
      "configure-build.sh requests the call media engine explicitly")
check("--call-media-status" in configure_src,
      "configure-build.sh probes the staged binary for the engine")
check("call media engine built in: yes" in configure_src,
      "configure-build.sh fails the build when the engine was configured out")

# The Flatpak does not use configure-build.sh, so its manifest repeats the guard.
flatpak_manifest = _read("packaging", "flatpak",
                         "org.lightning_matrix.Lightning.yaml.in")
_flatpak_code = "\n".join(
    line for line in flatpak_manifest.splitlines()
    if not line.lstrip().startswith("#"))
check("-DLIGHTNING_ENABLE_WEBRTC=ON" in _flatpak_code,
      "the Flatpak manifest requests the call media engine explicitly")
check("call media engine built in: yes" in _flatpak_code,
      "the Flatpak build fails when the engine was configured out")
# ...and fails at configure rather than after a full build.
check("-DLIGHTNING_REQUIRE_WEBRTC=ON" in _flatpak_code,
      "the Flatpak fails at configure, not after the build, like every other format")
check("-DLIGHTNING_REQUIRE_WEBRTC=ON" in configure_src,
      "configure-build.sh fails at configure when the probe finds nothing")

# --- gst-plugins-good's licence travels with its binaries -------------------
#
# gst-plugins-good is LGPL-2.1-or-later and the MinGW SDK does not ship its
# licence text, so the text is vendored here and staged by the AppImage (and
# the snap built from it) and Windows builds.
_good_license = os.path.join(HERE, "..", "packaging", "common", "licenses",
                             "gst-plugins-good-1.0", "COPYING")
check(os.path.isfile(_good_license),
      "the gst-plugins-good licence text is vendored in the repository")
with open(_good_license, encoding="utf-8") as handle:
    _good_text = handle.read()
check("GNU LESSER GENERAL PUBLIC LICENSE" in _good_text
      and "Version 2.1" in _good_text,
      "the vendored text is the LGPL 2.1")
for script, label in (("stage-windows-runtime.py", "the Windows stage"),
                      ("build-appimage.sh", "the AppImage build")):
    src = _read("scripts", script)
    check("gst-plugins-good-1.0" in src,
          f"{label} ships the gst-plugins-good licence")
# The macOS JPEG plugin (needed for the MJPG camera chain) is staged as
# optional first: a missing required plugin fails the allow_failure macOS job
# and would silently drop the macOS asset. Promote it once it stages.
_macos_stage = _strip_shell_comments(_read("scripts", "stage-macos-gstreamer.sh"))
check("OPTIONAL_PLUGINS=(jpeg)" in _macos_stage,
      "the macOS stage carries the JPEG plugin as OPTIONAL, not required")
check("TO PROMOTE IT:" in _read("scripts", "stage-macos-gstreamer.sh"),
      "the macOS stage records how that becomes required")
_macos_required = _macos_stage.split("PLUGINS=(")[1].split(")")[0]
check("jpeg" not in _macos_required.split(),
      "jpeg is not in the macOS REQUIRED plugin list yet")

for script, label in (("validate-windows-artifacts.sh", "the Windows validator"),
                      ("validate-appimage.sh", "the AppImage validator"),
                      ("validate-snap.sh", "the snap validator")):
    src = _strip_shell_comments(_read("scripts", script))
    check("gst-plugins-good-1.0" in src,
          f"{label} asserts the licence is in the extracted payload")
    check("GNU LESSER GENERAL PUBLIC LICENSE" in src,
          f"{label} checks the staged text is the LGPL, not just a file")

# --- licences for every bundled project -----------------------------------
#
# build-appimage.sh harvests /usr/share/doc/<pkg>/copyright for every Debian
# package owning a bundled object and fails on any it cannot attribute (the
# snap inherits this). build-macos.sh stages Lightning's GPL-3 text and the
# vendored gst-plugins-good licence.
_appimage_src = _strip_shell_comments(_read("scripts", "build-appimage.sh"))
check("/var/lib/dpkg/info" in _appimage_src,
      "the AppImage build derives licences from the dpkg file list, not a hand list")
check("usr/share/licenses/third-party" in _appimage_src,
      "the AppImage build stages a third-party licence directory")
# The path alone appears elsewhere in the script; assert the licence index
# covers the unpacked roots.
check("for unpacked in /opt/kimageformats /opt/pipewire-conf" in _appimage_src,
      "the licence index covers the packages the job UNPACKS rather than "
      "installs (kimg_jxl.so is in no dpkg file list, and an index built from "
      "/var/lib/dpkg alone would kill the job on it)")
for script, label in (("validate-appimage.sh", "the AppImage validator"),
                      ("validate-snap.sh", "the snap validator")):
    src = _strip_shell_comments(_read("scripts", script))
    check("usr/share/licenses/third-party" in src,
          f"{label} asserts the third-party licence directory is in the payload")
    check("-ge 100" in src,
          f"{label} asserts a COUNT, so a harvest that comes back short fails")
_macos_build = _strip_shell_comments(_read("scripts", "build-macos.sh"))
check("Lightning-GPL-3.0.txt" in _macos_build,
      "the macOS build stages Lightning's own GPL-3 text into the bundle")
check("gst-plugins-good-1.0" in _macos_build,
      "the macOS build stages the vendored gst-plugins-good licence")
_macos_validator = _strip_shell_comments(_read("scripts", "validate-macos-artifacts.sh"))
check("Lightning-GPL-3.0.txt" in _macos_validator,
      "the macOS validator asserts the GPL-3 text is in the BUNDLE")
check("gst-plugins-good-1.0" in _macos_validator,
      "the macOS validator asserts the gst-plugins-good licence is in the BUNDLE")

# --- 3. every format asks the shipped artifact ------------------------------
#
# Iterates FORMAT_SELECTOR, which is cross-checked against the file above.
for fmt in sorted(FORMAT_SELECTOR):
    validator = _strip_shell_comments(_read("scripts", f"validate-{fmt}.sh"))
    check("--call-media-status" in validator,
          f"validate-{fmt} runs the packaged build's own engine probe")
    check("assert_call_media_engine" in validator,
          f"validate-{fmt} judges the probe through the shared helper")
    # The voice-delay self-test: a default `queue` holds a second and never
    # leaks it, which no source check can see.
    check("--call-queue-selftest" in validator,
          f"validate-{fmt} measures the voice-delay property on the shipped artifact")
    check("assert_queue_selftest" in validator,
          f"validate-{fmt} judges that measurement through the shared helper")

# One helper judges every format: "compiled in" and "can run here" (plugins
# present) are both required.
lib_src = _strip_shell_comments(_read("scripts", "lib.sh"))
check("assert_call_media_engine()" in lib_src,
      "lib.sh defines the shared call-engine assertion")
check("call media engine built in: yes" in lib_src,
      "the shared assertion requires the engine to be compiled in")
check("RESULT: calls can be placed and answered." in lib_src,
      "the shared assertion requires the engine to be runnable")

# --call-media-status reports whether the compressed camera chain works by
# asking the registry, not by listing libgstjpeg. Reported, not required: a
# camera without it falls back to the slower raw chain.
with open(os.path.join(HERE, "..", "..", "src", "main.cpp"),
          encoding="utf-8") as handle:
    _main_cpp = handle.read()
check("camera compressed (MJPG) chain: " in _main_cpp,
      "--call-media-status reports whether the compressed camera chain works")
check("jpegCameraChainAvailable()" in _main_cpp,
      "it asks the engine's own probe rather than listing a file")

# The queue self-test: no verdict is a hard failure, while `fail` only warns
# until the check has passed everywhere (see TO PROMOTE IT in lib.sh).
check("assert_queue_selftest()" in lib_src,
      "lib.sh defines the shared voice-delay assertion")
# "Measured and failed" and "could not measure" must not share an exit path.
check("VERDICT: " in lib_src,
      "the shared assertion keys on an exact VERDICT line, not a prefix")
check("unmeasurable)" in lib_src,
      "the shared assertion has a distinct unmeasurable outcome")
check("measured NOTHING" in lib_src,
      "the shared assertion fails hard when nothing was measured")
# Wine output is CRLF, so the verdict would read `pass\r`.
check("tr -d " in lib_src,
      "the shared assertion strips CR before reading the verdict")
check("does not understand" in lib_src,
      "an unreadable verdict is reported as its own fault, not as a missing line")
# The non-Linux validators use the same helper from lib.sh.
for script in ("smoke-windows-wine.sh", "validate-macos-artifacts.sh"):
    src = _strip_shell_comments(_read("scripts", script))
    check("assert_queue_selftest" in src,
          f"{script} judges the self-test through the shared helper")
    check("timeout 300s" in src or "run_bounded 300" in src,
          f"{script} bounds the self-test in time")
# macOS has no GNU `timeout`.
check("run_bounded()" in lib_src,
      "lib.sh provides a time bound that does not need GNU coreutils")
check("gtimeout" in lib_src,
      "the bound tries Homebrew's coreutils name before falling back")
_macos_src2 = _strip_shell_comments(_read("scripts", "validate-macos-artifacts.sh"))
check("timeout 300s" not in _macos_src2,
      "the macOS validator does not call GNU timeout, which it has not got")
# Read the raw file: the promotion procedure is a comment.
check("TO PROMOTE IT:" in _read("scripts", "lib.sh"),
      "the shared assertion records how it becomes a hard gate")

# Windows and macOS have their own validators outside FORMAT_SELECTOR.
_wine_src = _strip_shell_comments(_read("scripts", "smoke-windows-wine.sh"))
check("--call-queue-selftest" in _wine_src,
      "the Windows package is asked the voice-delay question too")
_macos_src = _strip_shell_comments(_read("scripts", "validate-macos-artifacts.sh"))
check("--call-queue-selftest" in _macos_src,
      "the macOS bundle is asked the voice-delay question too")

# --- the call sounds, asked of every shipped artifact ----------------------
#
# Warn-only. Pins that every lane runs it and that the helper tells a measured
# shortfall from an unmeasured run: without an audio output device
# QSoundEffect loads nothing however healthy the package is.
for fmt in sorted(FORMAT_SELECTOR):
    validator = _strip_shell_comments(_read("scripts", f"validate-{fmt}.sh"))
    check("--call-sounds-status" in validator,
          f"validate-{fmt} asks the shipped artifact whether its call sounds load")
    check("assert_call_sounds_status" in validator,
          f"validate-{fmt} judges the call-sounds transcript through the shared helper")
for script in ("smoke-windows-wine.sh", "validate-macos-artifacts.sh"):
    src = _strip_shell_comments(_read("scripts", script))
    check("--call-sounds-status" in src,
          f"{script} asks the shipped artifact whether its call sounds load")
    check("assert_call_sounds_status" in src,
          f"{script} judges the call-sounds transcript through the shared helper")
check("timeout 60s wine64 \"$exe\" --call-sounds-status" in _read("scripts", "smoke-windows-wine.sh"),
      "the Windows call-sounds probe is bounded in time")
check("run_bounded 60 \"$CONTENTS/MacOS/$APP_NAME\" --call-sounds-status" in _macos_src,
      "the macOS call-sounds probe is bounded without GNU timeout")
check("assert_call_sounds_status()" in lib_src,
      "lib.sh defines the shared call-sounds assertion")
# The expected count must track data/sounds/, or a build that lost a sound
# would still read as complete.
_sounds_dir = os.path.join(HERE, "..", "..", "data", "sounds")
_wavs = sorted(n for n in os.listdir(_sounds_dir) if n.endswith(".wav"))
_expected = re.search(r"^CALL_SOUNDS_EXPECTED=(\d+)$", _read("scripts", "lib.sh"), re.M)
check(_expected is not None and int(_expected.group(1)) == len(_wavs),
      f"CALL_SOUNDS_EXPECTED matches the {len(_wavs)} sounds in data/sounds/")
# The helper parses the app's own words; pin that the app still says them.
check('"\\nRESULT: " << loaded << " of " << sounds.size()' in _main_cpp
      and '" call sounds loaded\\n"' in _main_cpp,
      "--call-sounds-status still prints the RESULT line the helper parses")
check('"default audio output: "' in _main_cpp,
      "--call-sounds-status still names the output device the helper keys on")


# Run the real helper over each transcript shape; a text scan cannot tell
# "UNMEASURED" from "MEASURED SHORT".
def _run_sounds_helper(transcript, status):
    import subprocess
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
        fh.write(transcript)
        path = fh.name
    try:
        proc = subprocess.run(
            ["bash", "-c",
             'source "$1"; assert_call_sounds_status TEST "$2" "$3"; echo "rc=$?"',
             "_", os.path.join(HERE, "..", "scripts", "lib.sh"), path, str(status)],
            capture_output=True, text=True, timeout=30)
        return proc.returncode, proc.stdout + proc.stderr
    finally:
        os.unlink(path)


_names = [n[:-4] for n in _wavs]
_all = "".join(f"  {n}: loaded\n" for n in _names)
_none = "".join(f"  {n}: NOT LOADED\n" for n in _names)
_n = len(_names)
_cases = [
    ("all loaded on a device",
     f"default audio output: Null Output\n{_all}\nRESULT: {_n} of {_n} call sounds loaded\n",
     0, f"TEST: call sounds: {_n} of {_n} loaded"),
    ("no output device at all",
     f"default audio output: none\n{_none}\nRESULT: 0 of {_n} call sounds loaded\n",
     1, "call sounds UNMEASURED"),
    ("a device, and a shortfall",
     f"default audio output: Speakers\r\n{_none}\r\nRESULT: 3 of {_n} call sounds loaded\r\n",
     1, "call sounds MEASURED SHORT"),
    ("no RESULT line", "Segmentation fault\n", 139, "printed no RESULT line"),
]
for _label, _text, _status, _want in _cases:
    _rc, _out = _run_sounds_helper(_text, _status)
    check(_rc == 0 and "rc=0" in _out and _want in _out,
          f"call-sounds helper, {_label}: warns only and says '{_want}'")

# --- the AppImage/snap bundle, which has nobody to depend on ----------------
appimage_build = _read("scripts", "build-appimage.sh")
# A missing plugin directory must fail, not silently skip staging.
check('[[ -d "$GST_PLUGIN_SRC" ]] || die' in appimage_build,
      "the AppImage fails rather than silently skipping plugin staging")
_appimage_plugin_list = re.search(
    r"GST_REQUIRED_PLUGINS=\((.*?)\n\)", appimage_build, re.S)
check(_appimage_plugin_list is not None,
      "build-appimage declares an explicit required-plugin list")
if _appimage_plugin_list:
    # Match the list, not the file: comments name the same plugins.
    staged = set(_appimage_plugin_list.group(1).split())
    for plugin in ("libgstwebrtc", "libgstnice", "libgstdtls", "libgstsrtp",
                   "libgstopus", "libgstrtp", "libgstvpx",
                   # In SfuMediaEngine's kRequired list.
                   "libgstvolume", "libgstaudiotestsrc", "libgstvideotestsrc",
                   # appsink/appsrc: the received-video path.
                   "libgstapp",
                   # Loaded by webrtcbin itself for the data channel that
                   # owns the bundled transport; without it nothing is received.
                   "libgstsctp",
                   # Screen share and camera.
                   "libgstpipewire", "libgstvideo4linux2",
                   # The X11 screen-share fallback. Not in kRequired, so
                   # --call-media-status cannot catch its absence, and the
                   # bundle replaces the system plugin path, hiding the host's.
                   "libgstximagesrc"):
        check(plugin in staged,
              f"the AppImage stages {plugin} into the AppDir")

# ximagesrc's load failure is silent, so both bundling validators resolve its
# libraries, with the host X libraries linuxdeploy leaves out installed.
for fmt in ("appimage", "snap"):
    validator = _strip_shell_comments(_read("scripts", f"validate-{fmt}.sh"))
    # Match the loop's own list: the `ldd` check below names the same file.
    _payload_loop = re.search(r"for gst_plugin in (.*?); do", validator, re.S)
    check(_payload_loop is not None,
          f"validate-{fmt} declares an explicit required-plugin loop")
    _payload_plugins = set(
        _payload_loop.group(1).replace("\\", " ").split()) if _payload_loop else set()
    check("libgstximagesrc" in _payload_plugins,
          f"validate-{fmt} requires the X11 screen-share fallback in the payload")
    check("not found" in validator,
          f"validate-{fmt} proves the staged fallback resolves its libraries")
    job_script = " ".join(resolve_extends(f"validate-{fmt}").get("before_script", []))
    for lib in ("libxdamage1", "libxfixes3", "libxtst6"):
        check(lib in job_script,
              f"validate-{fmt} provides the host library {lib} that ximagesrc links")

# The PipeWire client stack: libpipewire dlopens its own SPA plugins and
# modules from build-time paths and needs client.conf, or pipewiresrc fails at
# `pw_loop_new` and screen sharing is dead.
_appimage_src = _strip_shell_comments(_read("scripts", "build-appimage.sh"))
for _spa_dir in ("support", "videoconvert"):
    check(_spa_dir in _appimage_src.split("for spa_subdir in ")[1].split(";")[0]
          if "for spa_subdir in " in _appimage_src else False,
          f"the AppImage stages the spa-0.2/{_spa_dir} plugin directory")
# Match the list itself, not comments naming it.
_pw_loop = re.search(r"PW_REQUIRED_MODULES=\((.*?)\)", _appimage_src, re.S)
check(_pw_loop is not None,
      "build-appimage declares an explicit PipeWire module list")
_pw_modules = set(_pw_loop.group(1).split()) if _pw_loop else set()
# Debian's client.conf loads these without `ifexists nofail`, so a missing
# one makes pw_context_new() fail.
for _pw_module in ("libpipewire-module-protocol-native",
                   "libpipewire-module-client-node",
                   "libpipewire-module-client-device",
                   "libpipewire-module-adapter",
                   "libpipewire-module-metadata",
                   "libpipewire-module-session-manager"):
    check(_pw_module in _pw_modules,
          f"build-appimage stages the required PipeWire module {_pw_module}")
check("usr/share/pipewire/client.conf" in _appimage_src,
      "build-appimage stages pipewire client.conf, without which pw_context_new fails")
for _pw_var in ("SPA_PLUGIN_DIR", "PIPEWIRE_MODULE_DIR", "PIPEWIRE_CONFIG_DIR"):
    check(f'export {_pw_var}="$APPDIR/' in _appimage_src,
          f"the AppRun hook exports {_pw_var} so the staged stack is reachable")
_appimage_job = " ".join(resolve_extends("build-appimage").get("before_script", []))
for _pw_pkg in ("libspa-0.2-modules", "libpipewire-0.3-modules"):
    check(_pw_pkg in _appimage_job,
          f"build-appimage installs {_pw_pkg}, which gstreamer1.0-pipewire does not pull in")

# The engine probe only checks the `autodetect` factories, which exist with no
# sink installed. Debian ships the ALSA sink separately; Fedora does not.
check("gstreamer1.0-alsa" in _deb_deps if _deb_call else False,
      "deb depends on an ALSA sink, which Debian splits into its own package")
for needle in ("gstreamer1.0-pipewire",):
    check(needle in _deb_deps if _deb_call else False,
          f"deb depends on {needle} for the PipeWire sink and screen capture")

snap_build = _strip_shell_comments(_read("scripts", "build-snap.sh"))
# The snap takes only usr/ from the AppDir, so its launcher must set the
# plugin path the AppRun hook would have.
check("GST_PLUGIN_SYSTEM_PATH_1_0" in snap_build,
      "the snap launcher points GStreamer at the bundled plugins")
snap_yaml = _read("packaging", "snap", "snap.yaml.in")
_snap_plugs = [
    line.strip()[2:].strip()
    for line in snap_yaml.splitlines()
    if line.strip().startswith("- ") and not line.strip().startswith("- --")
]
# The microphone is a separate interface from audio-playback. Matched against
# list entries, since a comment names the same string.
check("audio-record" in _snap_plugs,
      "the snap declares the microphone interface calling needs")

with open(os.path.join(HERE, "..", "scripts", "validate-windows-artifacts.sh"),
          encoding="utf-8") as handle:
    win_validate_src = handle.read()
# An engine-less build still launches and syncs, so assert the linkage.
check("libgstwebrtc-1.0-0.dll" in win_validate_src,
      "Windows validation proves the application links the call media engine")
check("gst-element-probe.exe" in win_validate_src,
      "Windows validation runs the packaged tree's own element probe")

# --- Qt image-format plugins ------------------------------------------------
#
# Qt image formats are dlopen'd plugins, so packaging tools only deploy what
# qtbase carries (gif, ico, jpeg) while the client accepts webp. JPEG XL comes
# from KDE's kimageformats (kimg_jxl.so), available on Linux only.
# Comments are stripped before the source assertions below.

# 1. the AppImage job installs/unpacks what the AppImage stages.
_appimage_before = " ".join(resolve_extends("build-appimage").get("before_script", []))
for package in ("qt6-image-formats-plugins", "libjxl0.11",
                "kimageformat6-plugins"):
    check(package in _appimage_before,
          f"build-appimage obtains the image-format package {package}")

# 2. build-appimage.sh stages both plugins, declares them to linuxdeploy, and
#    checks the packed squashfs, not the AppDir it wrote.
_appimage_code = _strip_shell_comments(_read("scripts", "build-appimage.sh"))
check("usr/plugins/imageformats" in _appimage_code,
      "the AppImage stages Qt image-format plugins into the AppDir")
for plugin in ("libqwebp.so", "kimg_jxl.so"):
    check(plugin in _appimage_code,
          f"the AppImage stages {plugin}")
check("QT_IMAGE_REQUIRED_PLUGINS" in _appimage_code
      and "--library" in _appimage_code,
      "the staged image plugins are declared to linuxdeploy so their codec "
      "libraries are bundled too")
check(_appimage_code.count("verify_root/usr/plugins/imageformats") >= 2,
      "the PACKED AppImage is asked for its image-format plugins and for "
      "their dependency closure")

# 3. deliberate exclusions: avif pulls in AV1 encoders and abseil; heif's
#    libheif dlopens its own codecs, so it would register and decode nothing;
#    SVG must never reach a media path as active content (security rule).
for excluded in ("kimg_avif", "kimg_heif", "libqsvg"):
    check(excluded not in _appimage_code,
          f"the AppImage deliberately does not stage {excluded}")

# 4. deb and rpm DECLARE instead of bundling.
_deb_image = re.search(r'IMAGE_DEPENDS="([^"]*)"', deb_src)
check(_deb_image is not None, "build-deb declares IMAGE_DEPENDS")
if _deb_image:
    check("qt6-image-formats-plugins" in _deb_image.group(1),
          "deb depends on the Qt image-format plugins (webp)")
check("$IMAGE_DEPENDS" in deb_src,
      "build-deb actually writes IMAGE_DEPENDS into the control file")
_deb_recommends = re.search(r'IMAGE_RECOMMENDS="([^"]*)"', deb_src)
check(_deb_recommends is not None
      and "kimageformat6-plugins" in _deb_recommends.group(1),
      "deb recommends kimageformat6-plugins, the only Qt JPEG XL decoder")
check("Recommends: %s" in deb_src,
      "build-deb writes a Recommends field into the control file")

check("qt6-qtimageformats" in _rpm_requires,
      "rpm requires the Qt image-format plugins (webp)")
_rpm_recommends = "\n".join(
    line for line in spec_src.splitlines() if line.startswith("Recommends:"))
check("kf6-kimageformats" in _rpm_recommends,
      "rpm recommends kf6-kimageformats, the only Qt JPEG XL decoder")

# 5. the snap inherits the AppDir, so it asserts what it inherited.
_snap_code = _strip_shell_comments(_read("scripts", "build-snap.sh"))
for plugin in ("libqwebp.so", "kimg_jxl.so"):
    check(plugin in _snap_code,
          f"build-snap asserts the inherited AppDir carries {plugin}")

# 6. every validator asks the shipped artifact: a plugin present is not a
#    plugin that registers.
_IMAGE_VALIDATORS = {
    "validate-appimage.sh": True,
    "validate-deb.sh": True,
    "validate-rpm.sh": True,
    "validate-flatpak.sh": True,
    "validate-snap.sh": True,
    # No Qt JPEG XL plugin exists for Windows or macOS.
    "smoke-windows-wine.sh": False,
    "validate-macos-artifacts.sh": False,
}
for script, wants_jxl in _IMAGE_VALIDATORS.items():
    src = _strip_shell_comments(_read("scripts", script))
    # Join shell line continuations before searching.
    src = src.replace("\\\n", " ")
    check("--image-format-status" in src,
          f"{script} asks the shipped artifact which image formats it decodes")
    if wants_jxl:
        check(re.search(r'assert_image_formats [^\n]*\bjxl\b', src) is not None,
              f"{script} requires JPEG XL, which this platform can supply")
    else:
        check("jxl" not in src,
              f"{script} does not require JPEG XL (no Qt plugin exists for "
              f"this platform)")

# 7. the shared judgement names every required format individually.
_lib_code = _strip_shell_comments(_read("scripts", "lib.sh"))
check("assert_image_formats()" in _lib_code,
      "lib.sh carries one shared judgement of an --image-format-status run")
check("for fmt in png jpeg gif bmp webp" in _lib_code,
      "the shared judgement names each required format rather than trusting "
      "the RESULT line")

# 8. Windows stages the webp plugin in its hand-written plugin list.
check('"qwebp.dll"' in win_stage_src,
      "the Windows stage carries the WebP image-format plugin")

# --- supply-chain and secret-scope invariants --------------------------------

# 9. Every container image is pinned by digest: tags are mutable, and one of
#    these jobs decodes the update-signing key.
def _image_refs(node):
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "image":
                if isinstance(value, str):
                    yield value
                elif isinstance(value, dict) and isinstance(value.get("name"), str):
                    yield value["name"]
            else:
                yield from _image_refs(value)
    elif isinstance(node, list):
        for item in node:
            yield from _image_refs(item)

_images = list(_image_refs(doc))
check(len(_images) >= 10, f"found container images to check ({len(_images)})")
for ref in _images:
    # The Windows builder is built locally from packaging/windows/Dockerfile
    # (digest-pinned base) and never pulled, so it has no digest.
    if ref.startswith("lightning-windows-builder:"):
        continue
    check(re.search(r"@sha256:[0-9a-f]{64}$", ref) is not None,
          f"image {ref} is pinned by digest, not by tag")

_compose = _read("infrastructure", "windows-runner", "compose.yml")
check(re.search(r"gitlab/gitlab-runner@sha256:[0-9a-f]{64}", _compose) is not None,
      "the runner manager image is pinned by digest")

# 10. The signing key and mirror token are environment-scoped, so no build
#     job (running project CMake and build.rs code) receives them.
_sign = resolve_extends_dict(doc["sign-update-manifest"])
check(isinstance(_sign.get("environment"), dict)
      and _sign["environment"].get("name") == "signing",
      "sign-update-manifest declares the `signing` environment")
# resolve-source runs the full key check before any build, so it holds the
# key too, and must run no project build tooling.
_resolve = resolve_extends_dict(doc["resolve-source"])
check(isinstance(_resolve.get("environment"), dict)
      and _resolve["environment"].get("name") == "signing",
      "resolve-source declares the `signing` environment (fail-fast full check)")
_resolve_script = " ".join(str(x) for x in _resolve.get("script", []))
check("cmake" not in _resolve_script and "cargo" not in _resolve_script,
      "resolve-source runs no project-6 build tooling while holding the key")
_mirror = resolve_extends_dict(doc["mirror-release-to-github"])
check(isinstance(_mirror.get("environment"), dict)
      and _mirror["environment"].get("name") == "mirror",
      "mirror-release-to-github declares the `mirror` environment")
check(isinstance(_mirror.get("retry"), dict) and _mirror["retry"].get("max", 0) >= 1,
      "mirror-release-to-github is retried by the runner (its script is idempotent)")

# 16. The GitHub fallback slot is written after GitLab's promotion by a job
#     that cannot fail a completed release; the mirror token is checked before
#     anything is published.
_pre = resolve_extends_dict(doc["github-mirror-preflight"])
check(_pre.get("stage") == "resolve" and _pre.get("needs") == []
      and isinstance(_pre.get("environment"), dict)
      and _pre["environment"].get("name") == "mirror",
      "github-mirror-preflight runs first, alone, holding only the mirror token")
check(isinstance(_pre.get("retry"), dict) and _pre["retry"].get("max", 0) >= 1,
      "github-mirror-preflight is retried (one GitHub blip must not stop a release)")
# A manifest refresh must never depend on the GitHub token.
for _script in ("github-mirror-preflight.sh", "mirror-release-to-github.sh"):
    _src = _strip_shell_comments(_read("scripts", _script))
    check('"${UPDATE_REFRESH_LATEST_ONLY:-false}" == true' in _src,
          f"{_script} short-circuits a manifest refresh without contacting GitHub")
check("github-mirror-preflight" in needs_names("publish-packages"),
      "publish-packages needs the mirror preflight (a dead token stops the pipeline before publication)")
_slot = resolve_extends_dict(doc["mirror-update-manifest-to-github"])
check(_slot.get("stage") == "update" and _slot.get("allow_failure") is True
      and isinstance(_slot.get("retry"), dict) and _slot["retry"].get("max", 0) >= 1
      and isinstance(_slot.get("environment"), dict)
      and _slot["environment"].get("name") == "mirror",
      "mirror-update-manifest-to-github is allow_failure + retried and holds only the mirror token")
check("publish-update-manifest" in needs_names("mirror-update-manifest-to-github"),
      "the GitHub update slot is written only AFTER GitLab's latest promotion")
_slot_script = _strip_shell_comments(_read("scripts", "mirror-update-manifest-to-github.sh"))
check('make_latest:"false"' in _slot_script and 'prerelease:true' in _slot_script
      and "update-publication.json" in _slot_script,
      "the update slot never becomes GitHub's 'latest release' (prerelease + make_latest false) and requires GitLab's promotion record")
check('"$UPDATE_LATEST_TAG" == update-latest' in _slot_script,
      "the slot script refuses any slot name but the one the client compiles in")
_client_endpoints_path = os.path.join(HERE, "..", "..", "lightning", "src", "update", "UpdateEndpoints.cpp")
_client_endpoints = open(_client_endpoints_path).read() if os.path.exists(_client_endpoints_path) else ""
if _client_endpoints:
    check("releases/download/update-latest" in _client_endpoints,
          "the client's compiled-in fallback names the same update-latest slot this pipeline writes")
for job in ("build-deb", "build-rpm", "build-appimage", "build-flatpak",
            "build-snap", "build-windows"):
    check("environment" not in resolve_extends_dict(doc[job]),
          f"{job} declares no environment (and so receives no scoped secret)")
_validate = _strip_shell_comments(_read("scripts", "validate-release-request.sh"))
check("check-update-signing-keys.sh\" --public-only" in _validate
      and 'UPDATE_SIGNING_KEY_B64' in _validate,
      "resolve-source runs the public-only key check when the private key is scoped away")
_sign_script = _strip_shell_comments(_read("scripts", "sign-update-manifest.sh"))
check(re.search(r'check-update-signing-keys\.sh"?\s*$', _sign_script, re.M) is not None,
      "the signing job still runs the FULL key-consistency check")

# 11. The rustup installer is checksum-verified before it runs, everywhere.
_ci_text = _read(".gitlab-ci.yml")
check("https://sh.rustup.rs" not in _ci_text,
      "no job downloads the unverified sh.rustup.rs installer")
_rustup_runs = 0
for _name, _job in doc.items():
    if not isinstance(_job, dict) or "before_script" not in resolve_extends_dict(_job):
        continue
    _steps = [str(x) for x in resolve_extends_dict(_job)["before_script"]]
    _run = [i for i, st in enumerate(_steps) if "rustup-init -y" in st]
    if not _run:
        continue
    _rustup_runs += 1
    _chk = [i for i, st in enumerate(_steps) if "$RUSTUP_INIT_SHA256" in st and "sha256sum -c" in st]
    check(bool(_chk) and _chk[0] < _run[0],
          f"{_name}: the rustup-init sha256 check precedes the run, in the same before_script")
    check(not any("sh.rustup.rs" in st or "rustup-init.sh" in st for st in _steps),
          f"{_name}: no other rustup installer path is invoked")
check(_rustup_runs >= 3, f"rustup-init runs found and checked ({_rustup_runs})")
_flatpak = _read("packaging", "flatpak", "org.lightning_matrix.Lightning.yaml.in")
check("https://sh.rustup.rs" not in _flatpak
      and "sha256sum -c -" in _flatpak and "rustup/archive/" in _flatpak,
      "the Flatpak manifest pins and verifies rustup-init too")

# 12. The token-bearing GitLab API client never follows a redirect: curl
#     re-sends custom JOB-TOKEN/PRIVATE-TOKEN headers to a new host.
_api = _strip_shell_comments(_read("scripts", "gitlab-api.sh"))
_api_fn = _api[_api.index("api_request()"):_api.index("api_json_get()")]
check("--max-redirs 0" in _api_fn and "--location" not in _api_fn,
      "api_request sends the token with --max-redirs 0 and never --location")

# 13. The latest update slot cannot be rolled back without saying so.
_publish_update = _strip_shell_comments(_read("scripts", "publish-update-manifest.sh"))
check("UPDATE_ALLOW_LATEST_ROLLBACK" in _publish_update
      and "refusing to move the latest slot backwards" in _publish_update,
      "publish-update-manifest refuses a backwards latest promotion by default")

# 14. The NSIS installer validates both remembered install directories
#     (HKCU per-user, HKLM all-users) before trusting them.
_nsi = _read("packaging", "windows", "installer.nsi")
check(re.search(r"^\s*Function \.onInit", _nsi, re.M) is not None
      and re.search(r"^\s*Call DetectExistingInstalls", _nsi, re.M) is not None
      and '!define REG_APP "Software\\Mizerd\\Lightning"' in _nsi
      and 'ReadRegStr $ExistingUserDir HKCU "${REG_APP}" "InstallDir"' in _nsi
      and '${AndIfNot} ${FileExists} "$ExistingUserDir\\Lightning.exe"' in _nsi
      and 'ReadRegStr $ExistingMachineDir HKLM "${REG_APP}" "InstallDir"' in _nsi
      and '${AndIfNot} ${FileExists} "$ExistingMachineDir\\Lightning.exe"' in _nsi,
      "installer.nsi validates both registry install directories in .onInit")
# A silent install must be able to fail: `File /r` hits sharing violations on
# mapped files, and the updater trusts the exit code.
check("ClearErrors" in _nsi and "IfErrors" in _nsi,
      "installer.nsi checks whether writing its payload succeeded")
check("SetErrorLevel" in _nsi,
      "installer.nsi reports a failure through its exit code")
check(_nsi.count("IfErrors") >= 3,
      "installer.nsi checks the payload AND both marker writes")

# 15. Dockerfile digests are ENV, so --build-arg cannot disable a checksum.
_dockerfile = _read("packaging", "windows", "Dockerfile")
for name in ("QT_MULTIMEDIA_SHA256", "FFMPEG_SHA256", "GSTREAMER_SHA256"):
    check(re.search(rf"^ENV {name}=[0-9a-f]{{64}}$", _dockerfile, re.M) is not None
          and re.search(rf"^ARG {name}", _dockerfile, re.M) is None,
          f"{name} is an ENV in the Windows builder Dockerfile")

# 17. A script that reads RELEASE_TAG must call release_contract_env, which
#     sets it; under `set -u` a missing call fails only in a publishing run.
#     Keyed on RELEASE_TAG because write-build-info.sh sets PACKAGE_VERSION
#     itself.
_scripts_dir = os.path.join(HERE, "..", "scripts")
_readers = []
for _name in sorted(os.listdir(_scripts_dir)):
    if not _name.endswith(".sh") or _name == "gitlab-api.sh":
        continue
    with open(os.path.join(_scripts_dir, _name)) as _fh:
        _body = _fh.read()
    if re.search(r"\$\{?RELEASE_TAG\b", _body):
        _readers.append((_name, "release_contract_env" in _body))
check(_readers, "at least one packaging script reads RELEASE_TAG")
for _name, _calls in _readers:
    check(_calls, f"{_name} calls release_contract_env before reading RELEASE_TAG")

if errors:
    print(f"\nPipeline config tests FAILED ({len(errors)})", file=sys.stderr)
    sys.exit(1)
print("Pipeline config tests passed")
