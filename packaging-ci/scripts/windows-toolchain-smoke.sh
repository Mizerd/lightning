#!/usr/bin/env bash
set -Eeuo pipefail

TARGET=x86_64-pc-windows-gnu
tmp_dir="$(mktemp -d /tmp/lightning-windows-toolchain.XXXXXX)"
cleanup() {
    rm -rf -- "$tmp_dir"
}
trap cleanup EXIT

cat >"$tmp_dir/hello.cpp" <<'EOF'
#include <windows.h>
int main() { return sizeof(void *) == 8 ? 0 : 1; }
EOF
x86_64-w64-mingw32-g++ -O2 -o "$tmp_dir/hello-cpp.exe" "$tmp_dir/hello.cpp"

cat >"$tmp_dir/hello.rs" <<'EOF'
fn main() {
    assert_eq!(std::mem::size_of::<usize>(), 8);
}
EOF
/opt/rust/cargo/bin/rustc --target "$TARGET" -O \
    -C linker=x86_64-w64-mingw32-gcc \
    -o "$tmp_dir/hello-rust.exe" "$tmp_dir/hello.rs"

mkdir -p "$tmp_dir/qt"
cat >"$tmp_dir/qt/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(qt_windows_smoke LANGUAGES CXX)
find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS Core)
qt_add_executable(qt-windows-smoke main.cpp)
target_link_libraries(qt-windows-smoke PRIVATE Qt6::Core)
EOF
cat >"$tmp_dir/qt/main.cpp" <<'EOF'
#include <QCoreApplication>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    return sizeof(void *) == 8 ? 0 : 1;
}
EOF
cmake -S "$tmp_dir/qt" -B "$tmp_dir/qt-build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/workspace/packaging/windows/toolchain-mingw64.cmake \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$tmp_dir/qt-build" --parallel "${BUILD_JOBS:-4}"

pe_files=(
    "$tmp_dir/hello-cpp.exe"
    "$tmp_dir/hello-rust.exe"
    "$tmp_dir/qt-build/qt-windows-smoke.exe"
)
for pe in "${pe_files[@]}"; do
    [[ -f "$pe" ]]
    file "$pe"
    file "$pe" | grep -Eq 'PE32\+ executable.*\((console|GUI)\), x86-64'
    x86_64-w64-mingw32-objdump -f "$pe" | grep -F 'architecture: i386:x86-64'
done

printf 'Windows cross-toolchain smoke passed: MinGW C++, Rust %s, Qt %s\n' \
    "$(rustc --version)" "$(x86_64-w64-mingw32-qmake-qt6 -query QT_VERSION)"
