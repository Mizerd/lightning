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
required_stages = ["resolve", "build", "validate", "publish", "verify", "release"]
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
]
for job in required_jobs:
    check(job in doc, f"job {job} is defined")

# --- one dedicated runner per job: every build/validate job selects exactly
# --- one unique selector tag. build-appimage deliberately runs on the remote
# --- build lane (package-runner-remote on 10.195.35.6); its validation stays
# --- on the local appimage runner.
UNIQUE_SELECTORS = {"apt", "dnf", "nix", "flatpak", "appimage", "snap", "remote"}
BUILD_SELECTOR = {
    "deb": "apt", "rpm": "dnf",
    "flatpak": "flatpak", "appimage": "remote", "snap": "snap",
}
VALIDATE_SELECTOR = {
    "deb": "apt", "rpm": "dnf",
    "flatpak": "flatpak", "appimage": "appimage", "snap": "snap",
}
for fmt, selector in BUILD_SELECTOR.items():
    tags = set(doc["build-" + fmt].get("tags", []))
    check(tags & UNIQUE_SELECTORS == {selector},
          f"build-{fmt} selects exactly its own runner tag [{selector}]")
for fmt, selector in VALIDATE_SELECTOR.items():
    tags = set(doc["validate-" + fmt].get("tags", []))
    check(tags & UNIQUE_SELECTORS == {selector},
          f"validate-{fmt} selects exactly its own runner tag [{selector}]")



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


# --- serialization invariant: every build job shares one resource group so
# --- at most one package build runs at a time across ALL hosts (including
# --- the remote lane). Adding runners must never enable concurrent builds.
for fmt in BUILD_SELECTOR:
    merged = resolve_extends("build-" + fmt)
    check(merged.get("resource_group") == "lightning-package-build",
          f"build-{fmt} is serialized via the lightning-package-build resource group")

# --- publish-chain jobs route API requests through the internal endpoint
# --- (the public host is Cloudflare-proxied with a request-body cap that
# --- large package files exceed) ---
for job in ["publish-packages", "verify-published-packages", "finalize-release"]:
    merged = resolve_extends(job)
    base = (merged.get("variables") or {}).get("PUBLISH_API_BASE", "")
    check(base.startswith("http://10.195.35.2"),
          f"{job} sets PUBLISH_API_BASE to the internal GitLab endpoint")

# --- BUILD_JOBS values stay within the runner guard (configure-build.sh
# --- accepts 1-8) ---
default_jobs = str(doc.get("variables", {}).get("BUILD_JOBS", ""))
check(default_jobs.isdigit() and 1 <= int(default_jobs) <= 8,
      "default BUILD_JOBS is within 1-8")
for fmt in BUILD_SELECTOR:
    jobs = str((doc["build-" + fmt].get("variables") or {}).get("BUILD_JOBS", default_jobs))
    check(jobs.isdigit() and 1 <= int(jobs) <= 8,
          f"build-{fmt} BUILD_JOBS is within 1-8")

# --- workflow limited to web/api ---
wf = doc["workflow"]["rules"]
srcs = " ".join(str(r) for r in wf)
check('"web"' in srcs and '"api"' in srcs and "when: never" in yaml.dump(doc["workflow"]),
      "workflow accepts only web/api pipelines")

publish_jobs = ["publish-packages", "verify-published-packages", "finalize-release"]

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
v = dict(PUBLISH_PACKAGES="true", RELEASE_ACTION="create",
         CI_COMMIT_BRANCH="feature", CI_DEFAULT_BRANCH="main")
check(not evaluate(gate, v), "publish jobs excluded off the default branch")

if errors:
    print(f"\nPipeline config tests FAILED ({len(errors)})", file=sys.stderr)
    sys.exit(1)
print("Pipeline config tests passed")
