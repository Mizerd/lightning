import hashlib
import pathlib
import subprocess
import sys
import tempfile
import urllib.request

import yaml

MANIFEST = "org.lightning_matrix.Lightning.yaml"
OUTPUT = "cargo-sources.json"
MODULE = "lightning"
GENERATOR_REV = "41c20aa10819cdb2a4f3ca171758a96d1955c018"
GENERATOR_SHA256 = "0a2db6be87d75910facef28ab46d4d6460802e8419ab850d0caa6a364d26b380"
GENERATOR_URL = ("https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/"
                 f"{GENERATOR_REV}/cargo/flatpak-cargo-generator.py")

here = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
manifest = yaml.safe_load((here / MANIFEST).read_text())
modules = [m for m in manifest.get("modules", []) if isinstance(m, dict) and m.get("name") == MODULE]
if len(modules) != 1:
    sys.exit(f"expected exactly one module named {MODULE!r} in {MANIFEST}")
git_sources = [s for s in modules[0].get("sources", [])
               if isinstance(s, dict) and s.get("type") == "git"]
if len(git_sources) != 1:
    sys.exit(f"expected exactly one git source in module {MODULE!r}")
src = git_sources[0]
url, tag, commit = src.get("url"), src.get("tag"), src.get("commit")
if not (url and tag and commit and len(commit) == 40):
    sys.exit("the git source must carry url, tag and a full 40-character commit")

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    subprocess.run(["git", "-c", "advice.detachedHead=false", "clone", "--quiet",
                    "--depth", "1", "--branch", tag, "--", url, str(tmp / "src")], check=True)
    head = subprocess.run(["git", "-C", str(tmp / "src"), "rev-parse", "HEAD"],
                          check=True, capture_output=True, text=True).stdout.strip()
    if head != commit:
        sys.exit(f"tag {tag} is {head}, but the manifest pins {commit}")

    generator = urllib.request.urlopen(GENERATOR_URL, timeout=60).read()
    if hashlib.sha256(generator).hexdigest() != GENERATOR_SHA256:
        sys.exit("flatpak-cargo-generator does not match its pinned checksum")
    (tmp / "fcg.py").write_bytes(generator)

    subprocess.run([sys.executable, str(tmp / "fcg.py"), str(tmp / "src/rust/Cargo.lock"),
                    "-o", str(here / OUTPUT)], check=True)
print(f"{OUTPUT} regenerated from {tag} ({commit})")
