include("/usr/share/mingw/toolchain-mingw64.cmake")

# Fedora's MinGW Qt packages live below the target sysroot. Keep this explicit
# so a host Qt installation can never satisfy a Windows build.
list(PREPEND CMAKE_PREFIX_PATH
    "/usr/x86_64-w64-mingw32/sys-root/mingw"
)
