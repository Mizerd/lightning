# Lightning on NixOS

Lightning provides a package, a Home Manager module and a developer
shell. Choose the workflow you need:

| Use | Entry point |
| --- | --- |
| Install normally | `pkgs.lightning-matrix-client` through the overlay |
| Install with application debug symbols | `pkgs.lightning-matrix-client-debug` |
| Edit a clone and rebuild incrementally | `nix develop` or `nix/dev-shell.nix` |

The flake also exports both package names under `packages.<system>`;
`default` remains the normal package. Both packages install the same
`lightning-matrix` executable, so install only one variant. A developer
shell does not install Lightning; you build and run your checkout.

## Install with configuration.nix (without flakes)

Fetch a published Lightning revision containing the overlay. No source
checkout is needed for installation. In `configuration.nix`, replace
the two placeholders with the full commit and its unpacked hash:

```nix
{ pkgs, ... }:
let
  lightningRev = "<full-commit>";
  lightningSource = builtins.fetchTarball {
    url = "https://github.com/Mizerd/lightning/archive/${lightningRev}.tar.gz";
    sha256 = "<unpacked-sha256>";
  };
in
{
  nixpkgs.overlays = [ (import (lightningSource + "/nix/overlay.nix")) ];
  environment.systemPackages = [ pkgs.lightning-matrix-client ];
}
```

Obtain the unpacked hash for that same commit with:

```sh
nix-prefetch-url --unpack 'https://github.com/Mizerd/lightning/archive/<full-commit>.tar.gz'
```

This pins the application source while evaluating its package recipe
against the host's package set. Update the revision and hash together.
The package needs no developer shell or previously compiled executable.

For local packaging experiments, see the alternative using a
[checkout under ~/repos/lightning](#use-a-local-checkout), including
installation through this same configuration.nix workflow.

## Install with a system flake

Merge these entries into your existing NixOS flake. Keep your current
nixpkgs input, host name, platform and modules:

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    lightning = {
      url = "github:Mizerd/lightning";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { nixpkgs, lightning, ... }: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      modules = [
        ./configuration.nix
        ({ pkgs, ... }: {
          nixpkgs.overlays = [ lightning.overlays.default ];
          environment.systemPackages = [ pkgs.lightning-matrix-client ];
        })
      ];
    };

    # Optional: expose the development shell through this same lock file.
    devShells = lightning.devShells;
  };
}
```

The system lock file pins Lightning and nixpkgs. The overlay evaluates
the package recipe using the host's package set; `follows` also aligns
Lightning's flake packages and development shell with that nixpkgs
revision. Custom host overlays are not propagated by `follows`: if you
override Qt or the compiler, supply that same package set to the
[non-flake shell entry point](#develop-with-a-channel-based-host).

Build and activate through your normal NixOS configuration workflow,
then run `lightning-matrix` from your desktop or shell. No development
shell is needed for the installed package. Keep your existing graphics,
audio, portal and Secret Service configuration.

For Home Manager, retain the existing module and select the package
from the overlay (here `inputs` is supplied by your HM configuration):

```nix
{ inputs, pkgs, ... }:
{
  imports = [ inputs.lightning.homeManagerModules.default ];
  # Add this where HM's pkgs are constructed; with useGlobalPkgs, put it
  # in the NixOS configuration instead.
  nixpkgs.overlays = [ inputs.lightning.overlays.default ];
  lightning-matrix-client = {
    enable = true;
    package = pkgs.lightning-matrix-client;
  };
}
```

## Installed package with debug symbols

Replace the normal package with `pkgs.lightning-matrix-client-debug`
in either installation example, or set that as the Home Manager
module's `package`. For a standalone build from a checkout:

```sh
nix build .#lightning-matrix-client-debug --cores 8 --max-jobs 1
./result/bin/lightning-matrix
```

The debug package uses CMake `RelWithDebInfo`, retains symbols instead
of stripping the application, and builds Rust with optimization and
line information. It is larger than the normal package. Matching
symbols for Qt, PipeWire or other shared libraries must be obtained
separately; application symbols do not symbolize those libraries.

Standalone flake commands use the repository's nixpkgs pin. The
[graphics compatibility notes](#graphics-and-runtime-compatibility)
explain when to align it with the running NixOS system.

## Use a local checkout

Keep a development or local packaging checkout under your home directory:

```sh
mkdir -p ~/repos
git clone https://github.com/Mizerd/lightning.git ~/repos/lightning
cd ~/repos/lightning
```

If that checkout already exists, reuse it instead of cloning again.
Choose the branch or revision you want to build; the examples below
require a revision containing the overlay and developer helpers.

### Build a package from local Git sources

From the checkout, build and run the normal package:

```sh
nix build .#default --cores 8 --max-jobs 1
./result/bin/lightning-matrix
```

Use `.#lightning-matrix-client-debug` instead of `.#default` for the
debug package. These commands use the checkout's locked nixpkgs; see
[graphics compatibility](#graphics-and-runtime-compatibility) if it
differs from the running system. No developer shell is needed.

Nix builds a separate store package from a snapshot of your local Git
sources, including changes to files already known to Git. New source
files must be added to Git before these builds can see them. Repeating
the command after source changes creates a new package build; it does
not reuse your local CMake build directory. For frequent edits, use
the [incremental development workflow](#develop-and-rebuild-a-local-clone).

### Install local sources through configuration.nix

To build the checkout with the host's package set and install it as a
system package, replace the archive fetch in the installation example
with a local Git snapshot. Replace `alice` with your login name:

```nix
{ pkgs, ... }:
let
  lightningSource = builtins.fetchGit {
    url = "/home/alice/repos/lightning";
  };
in
{
  nixpkgs.overlays = [ (import (lightningSource + "/nix/overlay.nix")) ];
  environment.systemPackages = [ pkgs.lightning-matrix-client ];
}
```

With no `ref` or `rev`, this fetch uses the local files known to Git,
including uncommitted changes (see the
[fetchGit reference](https://nix.dev/manual/nix/latest/language/builtins.html#builtins-fetchGit)).
The Git snapshot excludes untracked build outputs. Keep the absolute path accessible to the account
evaluating your configuration, then use your usual NixOS rebuild
workflow. Each rebuild evaluates the checkout's current contents;
updating the checkout alone does not replace the installed package.
This path is specific to your machine. Use the pinned archive or
system-flake example when sharing a configuration across machines.

## Develop and rebuild a local clone

Use the checkout created above and enter its development shell:

```sh
cd ~/repos/lightning
env -u NIXPKGS_QT6_QML_IMPORT_PATH nix develop
```

CMake builds directly from this working directory, including new files.
The flake environment itself still needs any newly added Nix expressions
or helper scripts to be known to Git before `nix develop` can see them.

The examples use the default Bash shell provided by `nix develop`.
For a channel-based host, see the [non-flake shell instructions](#develop-with-a-channel-based-host).
If you exposed `devShells` from your system flake above, use
`env -u NIXPKGS_QT6_QML_IMPORT_PATH nix develop /etc/nixos`
instead to select its locked nixpkgs. Clearing the inherited desktop
QML path mirrors the developer launcher; the shell sets its own Qt
plugin paths.
The shell supplies dependencies; the working directory's source is
what CMake builds. Use a checkout compatible with that shell's pinned
Lightning revision.

The shell supplies `lightning-dev` and the Cargo/CMake job and debug
settings. Run these commands manually inside it:

```sh
lightning-dev configure
lightning-dev build
lightning-dev run
```

Run these commands from the checkout root. Entering the shell only
sets up the environment: it does not fetch Cargo sources, configure
CMake, compile, or launch the client.

`configure` fetches locked Cargo sources for CMake's offline build,
then configures a Debug build with the Rust backend, ccache, and mold
on Linux. Run it again after `Cargo.lock` changes. `build` compiles only
the application target; `run` launches the existing binary. Configure
accepts additional `-DNAME=VALUE` CMake cache definitions; run accepts
application arguments. Build uses the chosen job limit and takes no
extra arguments. For later source edits, repeat `lightning-dev build`
and restart the client.

The default build directory is `build-nixos`; set `LIGHTNING_BUILD_DIR`
to choose another tree. Set `CARGO_BUILD_JOBS` to a value from 1 to 8
to limit both C++ and Rust work; the shell defaults to eight. Configure
and build share a nonblocking `.developer-build.lock` inside that tree,
so a second helper refuses to overlap. Manual builds/tests must use
the same lock or wait until the tree is idle. See
[build performance](build-performance.md) for more about ccache and mold.

These are executable helpers, so `nix develop -c lightning-dev build`
also works without an interactive Bash session. Both the flake and
non-flake shell entry points provide the same helpers.

Keep the same build directory for incremental work. After changing the
toolchain or Qt, use a fresh directory and expect a full first build.
Nix package builds run separately and do not reuse a checkout's CMake
or Cargo outputs.

`lightning-dev run` uses the selected shell. `scripts/run-dev.sh`
enters the checkout's own flake environment again, so it can select a
different dependency pin from the host-aligned shell. The helper
also skips that script's optional GIF-key environment-file loading.

### Develop with a channel-based host

From a Lightning checkout, explicitly supply the same nixpkgs source
used to build the running system. For the root `nixos` channel:

```sh
env -u NIXPKGS_QT6_QML_IMPORT_PATH nix-shell -E 'import ./nix/dev-shell.nix { pkgs = import /nix/var/nix/profiles/per-user/root/channels/nixos {}; }'
```

Substitute your system's actual source; another user's channel can
point at a different revision. If the system applies compiler or Qt
overlays, apply those when constructing this `pkgs` too.

Alternatively, override the flake input for a local trial. Resolve the
channel symlink first, because some Nix implementations reject that
symlink as a flake path:

```sh
host_nixpkgs=$(readlink -f /nix/var/nix/profiles/per-user/root/channels/nixos)
env -u NIXPKGS_QT6_QML_IMPORT_PATH nix develop --no-write-lock-file --override-input nixpkgs "path:$host_nixpkgs"
```

An updated but unactivated channel may differ from the running system.
These commands do not update Lightning's committed lock file.

## Graphics and runtime compatibility

NixOS graphics drivers are loaded from `/run/opengl-driver` at runtime.
If Lightning's pinned environment is older than the running system,
the driver can require C/C++ runtime symbols that the application
process does not provide. Qt may then fail to create an EGL/OpenGL
context even though the GPU works in other applications.

Use the overlay with the host's package set, or align the developer
shell's nixpkgs as above and rebuild. Do not inject a newer libc into
an old process with `LD_PRELOAD` or `LD_LIBRARY_PATH`, or mix Qt plugins
from different versions. A system dependency override still needs to
satisfy Lightning's build requirements; arbitrary old stable channels
are not guaranteed to work.

In the chosen shell, `qtdiag` should report a real GPU and a working
OpenGL context. Its exit status alone is not sufficient: a failed
context probe can still exit zero. Then check Lightning's startup
renderer and test actual video and calls:

```sh
qtdiag
./build-nixos/lightning-matrix --call-media-status
./build-nixos/lightning-matrix --call-sounds-status
```

For an installed package, use `lightning-matrix` instead of the build
path. `Unknown hw device type "none"` concerns FFmpeg's hardware
decoding selection and does not by itself diagnose the EGL failure;
software decoding can coexist with GPU rendering.

See also the Nix manuals for
[overlays](https://nixos.org/manual/nixpkgs/unstable/#sec-overlays-definition)
and [flake inputs](https://nix.dev/manual/nix/latest/command-ref/new-cli/nix3-flake.html#flake-inputs).
