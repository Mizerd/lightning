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
# The mirror release must carry the RELEASE'S OWN NOTES, not only the "this is
# a mirror" notice. GitHub is where most readers land, and a page explaining
# what a mirror is while saying nothing about what changed is the wrong page.
# Both descriptions resolve from the same dist/release-notes.md that
# finalize-release uses for GitLab, so the two cannot drift.
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
WINDOWS_IMAGE = "lightning-windows-builder:fedora44-qt6.11.1-ffmpeg7.1.1-gst1.28.5-rust1.95.0-v5"
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
               "libgstwinks.dll", "libgstwinscreencap.dll",
               # sctp is here because its absence cost a whole release round.
               # NOTHING in Lightning names sctpenc — webrtcbin loads it for
               # the DATA CHANNEL, and LiveKit's subscriber offer puts one in
               # media section 0, which under bundle-policy=max-bundle owns
               # the transport every audio and video section rides on. Windows
               # shipped able to SEND and unable to RECEIVE anything, and the
               # element probe could not see it: a required-element list built
               # from what the application spells out cannot catch a plugin an
               # element loads on its own behalf.
               "libgstsctp.dll"):
    check(needle in win_stage_src, f"the Windows stage bundles {needle}")
# ...and the elements themselves, so staging the DLL without probing it is
# not enough. libgstsctp-1.0-0.dll — the SCTP LIBRARY — was copied all along
# while the plugin was missing; a name of the right shape is not the element.
for element in ("sctpenc", "sctpdec"):
    check(f'"{element}"' in win_stage_src,
          f"the Windows element probe covers {element}")

# ---------------------------------------------------------------------------
# The call media engine must be BUILT INTO every Linux package (2026-08-27).
#
# The section above pins the runtime DEPENDENCIES, and every one of them was
# correct while 0.8.0 shipped with calling compiled out of the binary they
# apply to. `--call-media-status` on the published deb answered "call media
# engine built in: no" and `ldd` named no GStreamer at all.
#
# The cause was silence. The source's LIGHTNING_ENABLE_WEBRTC defaults to ON,
# but it is only HONOURED when a pkg-config probe finds the GStreamer WebRTC
# development files -- and no Linux build job installed any, so CMake set
# HAVE_LIGHTNING_WEBRTC OFF, said so in one STATUS line among hundreds, and
# every downstream check still passed: the packages installed, launched,
# synced, and refused every call.
#
# Three things therefore have to hold, and this block pins all three:
#   1. every Linux job that COMPILES installs the development files;
#   2. the build asserts the resulting binary carries the engine;
#   3. every per-format validator asks the SHIPPED artifact whether calling
#      actually works.


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
    with open(os.path.join(HERE, "..", *parts), encoding="utf-8") as handle:
        return handle.read()


# --- 1. the development files, in every job that compiles -------------------
#
# build-flatpak and build-snap are deliberately absent: the Flatpak compiles
# INSIDE the org.kde.Sdk sandbox (which supplies all six pkg-config modules,
# verified against org.kde.Sdk//6.9) and the snap only repacks the AppImage
# job's AppDir. Neither runs a compiler in its own image.
_GST_DEV_PACKAGES = {
    # Debian/Ubuntu. gstreamer-1.0 comes from the first, sdp/app/video/rtp
    # from the second, and webrtc from the third -- all six modules the
    # source's pkg_check_modules names, verified in debian:13.6-slim.
    "build-deb": ("libgstreamer1.0-dev", "libgstreamer-plugins-base1.0-dev",
                  "libgstreamer-plugins-bad1.0-dev"),
    "build-appimage": ("libgstreamer1.0-dev", "libgstreamer-plugins-base1.0-dev",
                       "libgstreamer-plugins-bad1.0-dev"),
    # Fedora, same split, verified in fedora:44.
    "build-rpm": ("gstreamer1-devel", "gstreamer1-plugins-base-devel",
                  "gstreamer1-plugins-bad-free-devel"),
}
for job, packages in _GST_DEV_PACKAGES.items():
    # The parsed before_script carries only COMMANDS -- YAML has already
    # dropped the comments that name these same packages.
    script_text = " ".join(resolve_extends(job).get("before_script", []))
    for package in packages:
        check(package in script_text,
              f"{job} installs the GStreamer dev package {package}")

# The AppImage BUNDLES the runtime plugins instead of depending on them (the
# snap then inherits that AppDir), so its job needs them installed as well.
_appimage_before = " ".join(resolve_extends("build-appimage").get("before_script", []))
for package in ("gstreamer1.0-plugins-base", "gstreamer1.0-plugins-good",
                "gstreamer1.0-plugins-bad", "gstreamer1.0-nice",
                "gstreamer1.0-pipewire", "gstreamer1.0-alsa"):
    check(package in _appimage_before,
          f"build-appimage installs the runtime plugin package {package}")

# --- 2. the build refuses to produce an engine-less binary ------------------
configure_src = _strip_shell_comments(_read("scripts", "configure-build.sh"))
check("-DLIGHTNING_ENABLE_WEBRTC=ON" in configure_src,
      "configure-build.sh requests the call media engine explicitly")
check("--call-media-status" in configure_src,
      "configure-build.sh probes the staged binary for the engine")
check("call media engine built in: yes" in configure_src,
      "configure-build.sh fails the build when the engine was configured out")

# The Flatpak is the ONE packaging build that does not run configure-build.sh,
# so the same guard has to be spelled out in its manifest.
flatpak_manifest = _read("packaging", "flatpak",
                         "org.lightning_matrix.Lightning.yaml.in")
_flatpak_code = "\n".join(
    line for line in flatpak_manifest.splitlines()
    if not line.lstrip().startswith("#"))
check("-DLIGHTNING_ENABLE_WEBRTC=ON" in _flatpak_code,
      "the Flatpak manifest requests the call media engine explicitly")
check("call media engine built in: yes" in _flatpak_code,
      "the Flatpak build fails when the engine was configured out")
# ...and fails FAST, at configure, rather than after a full Rust build. Every
# other format gets this from configure-build.sh.
check("-DLIGHTNING_REQUIRE_WEBRTC=ON" in _flatpak_code,
      "the Flatpak fails at configure, not after the build, like every other format")
check("-DLIGHTNING_REQUIRE_WEBRTC=ON" in configure_src,
      "configure-build.sh fails at configure when the probe finds nothing")

# --- 3. every format asks the SHIPPED artifact ------------------------------
#
# Derived from the format list above rather than written out, so a new Linux
# format cannot be added without this check coming with it.
for fmt in sorted(FORMAT_SELECTOR):
    validator = _strip_shell_comments(_read("scripts", f"validate-{fmt}.sh"))
    check("--call-media-status" in validator,
          f"validate-{fmt} runs the packaged build's own engine probe")
    check("assert_call_media_engine" in validator,
          f"validate-{fmt} judges the probe through the shared helper")

# ONE helper judges all five, so the bar cannot drift between formats. Both
# halves matter: the first line answers "was it compiled in", the RESULT line
# answers "can it actually run here" -- an engine compiled in with no plugins
# beside it refuses calls exactly as completely as no engine at all.
lib_src = _strip_shell_comments(_read("scripts", "lib.sh"))
check("assert_call_media_engine()" in lib_src,
      "lib.sh defines the shared call-engine assertion")
check("call media engine built in: yes" in lib_src,
      "the shared assertion requires the engine to be compiled in")
check("RESULT: calls can be placed and answered." in lib_src,
      "the shared assertion requires the engine to be runnable")

# --- the AppImage/snap bundle, which has nobody to depend on ----------------
appimage_build = _read("scripts", "build-appimage.sh")
# Staging used to be wrapped in `if [ -d "$GST_PLUGIN_SRC" ]`, and the job
# installed no GStreamer, so the whole block was skipped on EVERY build and
# the pipeline stayed green. Absence must be loud.
check('[[ -d "$GST_PLUGIN_SRC" ]] || die' in appimage_build,
      "the AppImage fails rather than silently skipping plugin staging")
_appimage_plugin_list = re.search(
    r"GST_REQUIRED_PLUGINS=\((.*?)\n\)", appimage_build, re.S)
check(_appimage_plugin_list is not None,
      "build-appimage declares an explicit required-plugin list")
if _appimage_plugin_list:
    # Matched against the LIST, not the file: every name below also appears in
    # the comment above it explaining why it is there.
    staged = set(_appimage_plugin_list.group(1).split())
    for plugin in ("libgstwebrtc", "libgstnice", "libgstdtls", "libgstsrtp",
                   "libgstopus", "libgstrtp", "libgstvpx",
                   # In SfuMediaEngine's kRequired list: the engine REFUSES
                   # without these, and the pre-2026-08-27 staging list had
                   # none of the four.
                   "libgstvolume", "libgstaudiotestsrc", "libgstvideotestsrc",
                   # appsink/appsrc: the received-video path.
                   "libgstapp",
                   # Named NOWHERE in Lightning -- webrtcbin loads it itself
                   # for the data channel, which under bundle-policy=max-bundle
                   # owns the transport every media section rides on. Windows
                   # shipped for months able to send and unable to receive
                   # because this plugin was not staged.
                   "libgstsctp",
                   # Screen share and camera.
                   "libgstpipewire", "libgstvideo4linux2",
                   # The X11 screen-share fallback, and the one entry here
                   # that NO runtime check can defend. It is deliberately not
                   # in the engine's kRequired list -- a call does not need it
                   # -- so `--call-media-status` is green on a bundle without
                   # it while the feature is dead: SfuCallController probes the
                   # RUNNING REGISTRY, and the AppRun hook and snap launcher
                   # REPLACE the system plugin path, so the host's
                   # plugins-good is invisible. The user is then told to
                   # install a package they probably already have.
                   "libgstximagesrc"):
        check(plugin in staged,
              f"the AppImage stages {plugin} into the AppDir")

# Staging a plugin that cannot load is staging nothing, and ximagesrc is the
# only one whose failure is silent end to end. Both bundling formats resolve it
# the way the loader will, and their jobs install the X libraries linuxdeploy
# deliberately leaves on the host.
for fmt in ("appimage", "snap"):
    validator = _strip_shell_comments(_read("scripts", f"validate-{fmt}.sh"))
    # Matched against the LOOP'S OWN LIST, not the file. A bare substring search
    # passed with the name deleted from the loop, because the `ldd` check below
    # it names the same file -- so the assertion was true for a reason that had
    # nothing to do with what it claimed. Caught by mutation, not by review.
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

# THE PIPEWIRE CLIENT STACK. libgstpipewire being staged proved nothing: the
# plugin registered, built a pipeline, and died at `pw_loop_new: can't make
# support.system handle` because libpipewire dlopens its OWN SPA plugins and
# modules from paths compiled in at build time, and reads its module list from a
# config file it has no fallback for. Screen sharing was dead in 0.8.0 and in
# pipeline 142 while audio worked BOTH WAYS and video RECEIVE worked, which is
# what made it look like anything but a missing directory.
_appimage_src = _strip_shell_comments(_read("scripts", "build-appimage.sh"))
for _spa_dir in ("support", "videoconvert"):
    check(_spa_dir in _appimage_src.split("for spa_subdir in ")[1].split(";")[0]
          if "for spa_subdir in " in _appimage_src else False,
          f"the AppImage stages the spa-0.2/{_spa_dir} plugin directory")
# Matched against the LOOP'S OWN LIST, exactly as the ximagesrc case above
# learned to be: a bare substring search is satisfied by the comment naming it.
_pw_loop = re.search(r"PW_REQUIRED_MODULES=\((.*?)\)", _appimage_src, re.S)
check(_pw_loop is not None,
      "build-appimage declares an explicit PipeWire module list")
_pw_modules = set(_pw_loop.group(1).split()) if _pw_loop else set()
# Six of the seven are HARD-REQUIRED: Debian's client.conf lists them without
# `flags = [ ifexists nofail ]`, so a missing one makes pw_context_new() return
# NULL rather than degrade.
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

# An installed package must be able to make a SOUND. `autoaudiosink` resolves to
# pipewiresink, pulsesink or alsasink, and the engine's probe only asks for the
# `autodetect` FACTORIES -- which exist whether or not any sink is installed --
# so --call-media-status is green on a package with none. Debian splits ALSA into
# its own binary package; Fedora does not (base carries libgstalsa, good carries
# libgstpulseaudio), which is why only the deb needs the extra name.
check("gstreamer1.0-alsa" in _deb_deps if _deb_call else False,
      "deb depends on an ALSA sink, which Debian splits into its own package")
for needle in ("gstreamer1.0-pipewire",):
    check(needle in _deb_deps if _deb_call else False,
          f"deb depends on {needle} for the PipeWire sink and screen capture")

snap_build = _strip_shell_comments(_read("scripts", "build-snap.sh"))
# The snap takes only usr/ from the AppDir, so linuxdeploy's AppRun and its
# apprun-hooks/gstreamer.sh stay behind and the launcher is the ONLY thing that
# can point GStreamer at the bundled plugins.
check("GST_PLUGIN_SYSTEM_PATH_1_0" in snap_build,
      "the snap launcher points GStreamer at the bundled plugins")
snap_yaml = _read("packaging", "snap", "snap.yaml.in")
_snap_plugs = [
    line.strip()[2:].strip()
    for line in snap_yaml.splitlines()
    if line.strip().startswith("- ") and not line.strip().startswith("- --")
]
# audio-playback alone is a call nobody can hear the user on; the microphone is
# a separate snap interface. Matched against actual list entries, not the file,
# because the comment beside it names the same string.
check("audio-record" in _snap_plugs,
      "the snap declares the microphone interface calling needs")

with open(os.path.join(HERE, "..", "scripts", "validate-windows-artifacts.sh"),
          encoding="utf-8") as handle:
    win_validate_src = handle.read()
# The ONE fact no file listing can show: an engine-less build is a normal,
# launchable, syncing package.
check("libgstwebrtc-1.0-0.dll" in win_validate_src,
      "Windows validation proves the application links the call media engine")
check("gst-element-probe.exe" in win_validate_src,
      "Windows validation runs the packaged tree's own element probe")

# --- Qt image-format plugins ------------------------------------------------
#
# THE SAME DEFECT AS THE CALL PLUGINS, one layer up. A Qt image format is a
# dlopen'd plugin, so ELF NEEDED entries name none of them: dpkg-shlibdeps,
# rpm's generator and linuxdeploy-plugin-qt all deploy or declare only what
# qtbase itself carries -- libqgif, libqico, libqjpeg. Every Linux package up
# to and including 0.8.0 shipped exactly those three while the client's OWN
# byte sniffers ACCEPTED image/webp, so it accepted, forwarded and re-uploaded
# a format it could not draw. Verified on the shipped artifact: 0.8.0's
# usr/plugins/imageformats holds three files.
#
# JPEG XL is the reported symptom and does NOT come from Qt: qtimageformats has
# never contained a JXL plugin, so kimg_jxl.so from KDE's kimageformats is the
# only implementation, and it exists for Linux alone.
#
# Comments are stripped before every source assertion below, because each one
# of these scripts explains itself using the very strings asserted.

# 1. the AppImage job installs/unpacks what the AppImage stages.
_appimage_before = " ".join(resolve_extends("build-appimage").get("before_script", []))
for package in ("qt6-image-formats-plugins", "libjxl0.11",
                "kimageformat6-plugins"):
    check(package in _appimage_before,
          f"build-appimage obtains the image-format package {package}")

# 2. build-appimage.sh stages both plugins, declares them to linuxdeploy, and
#    asks the PACKED squashfs -- not the AppDir it wrote itself.
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

# 3. the deliberate exclusions. avif drags three AV1 encoders and ~20 abseil
#    libraries; heif needs libheif, which DLOPENS its own codec plugins, so a
#    staged kimg_heif.so would register the format and decode nothing; SVG must
#    never reach a media path as active content (Lightning CLAUDE.md §6).
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

# 6. EVERY validator asks the SHIPPED artifact, because a plugin present is not
#    a plugin that registers -- the lesson libgstsctp.dll and the PipeWire SPA
#    modules each taught this repository once.
_IMAGE_VALIDATORS = {
    "validate-appimage.sh": True,
    "validate-deb.sh": True,
    "validate-rpm.sh": True,
    "validate-flatpak.sh": True,
    "validate-snap.sh": True,
    # Windows and macOS: no Qt JPEG XL plugin exists for either platform, so
    # they assert the required set and leave JXL reported as a platform limit.
    "smoke-windows-wine.sh": False,
    "validate-macos-artifacts.sh": False,
}
for script, wants_jxl in _IMAGE_VALIDATORS.items():
    src = _strip_shell_comments(_read("scripts", script))
    # Join shell line continuations: these calls wrap, and a line-anchored
    # search would report the argument absent on a correct tree.
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

# 7. the shared judgement names every required format individually. A bare
#    RESULT check would pass on a table that had quietly demoted one.
_lib_code = _strip_shell_comments(_read("scripts", "lib.sh"))
check("assert_image_formats()" in _lib_code,
      "lib.sh carries one shared judgement of an --image-format-status run")
check("for fmt in png jpeg gif bmp webp" in _lib_code,
      "the shared judgement names each required format rather than trusting "
      "the RESULT line")

# 8. Windows stages the webp plugin in its hand-written plugin list.
check('"qwebp.dll"' in win_stage_src,
      "the Windows stage carries the WebP image-format plugin")

# --- supply-chain and secret-scope invariants (2026-09-02 security audit) ---

# 9. Every container image is pinned by DIGEST. A mutable tag such as
#    alpine:3.22 is re-published on every point release, and one of these
#    jobs decodes the update-signing key.
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
    # The Windows builder is built BY HAND on the runner host from
    # packaging/windows/Dockerfile (itself digest-pinned to its base) and
    # never pulled from a registry, so a digest is not a thing it has.
    if ref.startswith("lightning-windows-builder:"):
        continue
    check(re.search(r"@sha256:[0-9a-f]{64}$", ref) is not None,
          f"image {ref} is pinned by digest, not by tag")

_compose = _read("infrastructure", "windows-runner", "compose.yml")
check(re.search(r"gitlab/gitlab-runner@sha256:[0-9a-f]{64}", _compose) is not None,
      "the runner manager image is pinned by digest")

# 10. The private signing key and the mirror token are ENVIRONMENT-SCOPED, so
#     they are injected only into the job that declares the environment and
#     never into a build job that runs project-6 CMake and every build.rs.
_sign = resolve_extends_dict(doc["sign-update-manifest"])
check(isinstance(_sign.get("environment"), dict)
      and _sign["environment"].get("name") == "signing",
      "sign-update-manifest declares the `signing` environment")
# resolve-source runs the FULL key check before any build (the fail-fast
# property), so it must hold the key too -- and it executes no project-6 code.
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

# 11. The rustup installer is checksum-verified before it runs, everywhere it
#     runs. It was the one unverified executable in the pipeline.
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
#     re-sends a custom JOB-TOKEN/PRIVATE-TOKEN header to a new host.
_api = _strip_shell_comments(_read("scripts", "gitlab-api.sh"))
_api_fn = _api[_api.index("api_request()"):_api.index("api_json_get()")]
check("--max-redirs 0" in _api_fn and "--location" not in _api_fn,
      "api_request sends the token with --max-redirs 0 and never --location")

# 13. The latest update slot cannot be rolled back without saying so.
_publish_update = _strip_shell_comments(_read("scripts", "publish-update-manifest.sh"))
check("UPDATE_ALLOW_LATEST_ROLLBACK" in _publish_update
      and "refusing to move the latest slot backwards" in _publish_update,
      "publish-update-manifest refuses a backwards latest promotion by default")

# 14. The NSIS installer VALIDATES the HKCU install directory (user-writable,
#     and there is no directory page) before trusting it.
_nsi = _read("packaging", "windows", "installer.nsi")
check(re.search(r"^\s*Function \.onInit", _nsi, re.M) is not None
      and 'IfFileExists "$INSTDIR\\Lightning.exe"' in _nsi
      and 'ReadRegStr $0 HKCU "Software\\Mizerd\\Lightning" "InstallDir"' in _nsi,
      "installer.nsi validates the registry install directory in .onInit")

# 15. Dockerfile digests are ENV, so --build-arg cannot disable a checksum.
_dockerfile = _read("packaging", "windows", "Dockerfile")
for name in ("QT_MULTIMEDIA_SHA256", "FFMPEG_SHA256", "GSTREAMER_SHA256"):
    check(re.search(rf"^ENV {name}=[0-9a-f]{{64}}$", _dockerfile, re.M) is not None
          and re.search(rf"^ARG {name}", _dockerfile, re.M) is None,
          f"{name} is an ENV in the Windows builder Dockerfile")

if errors:
    print(f"\nPipeline config tests FAILED ({len(errors)})", file=sys.stderr)
    sys.exit(1)
print("Pipeline config tests passed")
