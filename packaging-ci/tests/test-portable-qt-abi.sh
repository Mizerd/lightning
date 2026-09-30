#!/usr/bin/env bash
set -Eeuo pipefail

# assert_portable_qt_abi (lib.sh) is what lets one .rpm install on Fedora and
# openSUSE: no Qt private-ABI import, and the Qt version tag kept. Its input is
# `objdump -T`, stubbed here with lines in the format measured on real builds
# on 2026-09-29: the AOT build imports two
# QQmlPrivate::AOTCompiledContext::mark symbols, tagged Qt_6.11_PRIVATE_API by
# Fedora's Qt, Qt_6.11.2_PRIVATE_API by openSUSE's and Qt_6_PRIVATE_API by the
# Nix dev shell's; a bytecode-only build imports none.

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LIB="$HERE/../scripts/lib.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
ok() { printf '  ok: %s\n' "$1"; }
bad() { printf '  FAIL: %s\n' "$1" >&2; fail=1; }

mkdir -p "$WORK/bin" "$WORK/fx"
# objdump -T <binary> prints the fixture named after the binary.
cat >"$WORK/bin/objdump" <<STUB
#!/usr/bin/env bash
[[ "\$1" == -T ]] || exit 2
cat "$WORK/fx/\$(basename "\$2").dynsym"
STUB
chmod +x "$WORK/bin/objdump"

imports() { # count
    local i
    printf '\n%s:     file format elf64-x86-64\n\nDYNAMIC SYMBOL TABLE:\n' lightning-matrix
    for ((i = 0; i < $1; i++)); do
        printf '0000000000000000      DF *UND*\t0000000000000000 (Qt_6)       _ZN7QObject%dE\n' "$i"
    done
}
tag='0000000000000000      DO *UND*	0000000000000000 (Qt_6.11)    qt_version_tag'
aot() { # tag
    printf '0000000000000000      DF *UND*\t0000000000000000 (%s) _ZN11QQmlPrivate18AOTCompiledContext4markEP7QObjectPN3QV49MarkStackE\n' "$1"
    printf '0000000000000000      DF *UND*\t0000000000000000 (%s) _ZN11QQmlPrivate18AOTCompiledContext4markERK8QVariantPN3QV49MarkStackE\n' "$1"
}
{ imports 1388; printf '%s\n' "$tag"; } >"$WORK/fx/bytecode.dynsym"
{ imports 1388; printf '%s\n' "$tag"; aot Qt_6.11_PRIVATE_API; } >"$WORK/fx/aot-fedora.dynsym"
{ imports 1388; printf '%s\n' "$tag"; aot Qt_6.11.2_PRIVATE_API; } >"$WORK/fx/aot-opensuse.dynsym"
{ imports 1388; printf '%s\n' "$tag"; aot Qt_6_PRIVATE_API; } >"$WORK/fx/aot-nix.dynsym"
# The first prototype also dropped Qt's version tagging.
imports 1318 >"$WORK/fx/untagged.dynsym"
imports 40 >"$WORK/fx/short.dynsym"
: >"$WORK/fx/empty.dynsym"

printf 'libQt6Core.so.6()(64bit)\nlibQt6Core.so.6(Qt_6)(64bit)\nlibQt6Core.so.6(Qt_6.11)(64bit)\nlibQt6Qml.so.6(Qt_6)(64bit)\n' \
    >"$WORK/requires-good.txt"
cat "$WORK/requires-good.txt" >"$WORK/requires-private.txt"
printf 'libQt6Qml.so.6(Qt_6.11_PRIVATE_API)(64bit)\n' >>"$WORK/requires-private.txt"
grep -v 'Qt_6.11' "$WORK/requires-good.txt" >"$WORK/requires-untagged.txt"

check() { # name binary requires expect(pass|fail) [message]
    local name="$1" binary="$2" requires="$3" expect="$4" want="${5:-}" rc=0
    ( export PATH="$WORK/bin:$PATH"
      # shellcheck source=../scripts/lib.sh
      source "$LIB"
      assert_portable_qt_abi RPM "$WORK/$binary" "$WORK/$requires" "$WORK/out.txt" ) \
        >"$WORK/log.txt" 2>&1 || rc=$?
    if [[ "$expect" == pass ]]; then
        [[ "$rc" == 0 ]] && ok "$name" || bad "$name: refused ($(tail -1 "$WORK/log.txt"))"
    elif [[ "$rc" == 0 ]]; then
        bad "$name: accepted"
    elif grep -qF "$want" "$WORK/log.txt"; then
        ok "$name"
    else
        bad "$name: refused for the wrong reason ($(tail -1 "$WORK/log.txt"))"
    fi
}

printf '== portable Qt ABI ==\n'
check "a bytecode-only build passes" bytecode requires-good.txt pass
check "a Fedora-tagged AOT import is refused" aot-fedora requires-good.txt fail \
    "imports 2 Qt private-ABI symbols"
check "an openSUSE-tagged AOT import is refused" aot-opensuse requires-good.txt fail \
    "imports 2 Qt private-ABI symbols"
check "a Nix-tagged AOT import is refused" aot-nix requires-good.txt fail \
    "imports 2 Qt private-ABI symbols"
check "a build without Qt's version tag is refused" untagged requires-good.txt fail \
    "lost qt_version_tag"
check "a package requiring a private version node is refused" bytecode requires-private.txt fail \
    "requires a Qt private-ABI version node"
check "a package without its Qt minor floor is refused" bytecode requires-untagged.txt fail \
    "does not require the Qt minor"
check "a listing too short to mean anything is refused" short requires-good.txt fail \
    "would prove nothing"
check "an empty listing is refused" empty requires-good.txt fail "would prove nothing"
check "the listing is kept for the job's artifacts" bytecode requires-good.txt pass
[[ -s "$WORK/out.txt" ]] && ok "objdump's listing was written where asked" \
    || bad "objdump's listing was not written"

if [[ "$fail" == 0 ]]; then printf 'Portable Qt ABI tests passed\n'; else printf 'Portable Qt ABI tests FAILED\n' >&2; exit 1; fi
