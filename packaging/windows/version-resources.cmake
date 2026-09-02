# Per-target Windows version resources for the Lightning cross-build.
#
# Injected into the UNMODIFIED Lightning source configure with
# -DCMAKE_PROJECT_INCLUDE=<this file>, which CMake includes immediately after
# the top-level project() call. This is packaging configuration, exactly like
# the MinGW toolchain file and the macOS -framework link inputs; the application
# source is not patched.
#
# Why this exists at all:
#
#   build-windows.sh used to hand the compiled VERSIONINFO/ICON object to
#   -DCMAKE_EXE_LINKER_FLAGS, which is GLOBAL. With one executable that was
#   fine. Since Lightning ships a second one (lightning-updater), a global
#   resource would stamp lightning-updater.exe with OriginalFilename
#   "Lightning.exe" and ProductName "Lightning" -- metadata that is simply
#   false, that SignPath's file-metadata restrictions apply to, and that
#   verify-windows-metadata.py rejects in both directions (as a Lightning-owned
#   file whose OriginalFilename does not match its name, or as a non-owned file
#   claiming to be Lightning).
#
#   CMake has no command-line way to set per-target link options, so the
#   assignment is deferred to the end of the top-level directory scope, at which
#   point both targets exist. Nothing else about the build is touched.
#
# Inputs (all set by scripts/build-windows.sh):
#   LIGHTNING_APP_VERSION_OBJECT      windres object for lightning-matrix
#   LIGHTNING_UPDATER_VERSION_OBJECT  windres object for lightning-updater

if(CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR
        "cmake_language(DEFER) needs CMake 3.19+; the Windows builder image has "
        "${CMAKE_VERSION}. Update the image or restore a global "
        "CMAKE_EXE_LINKER_FLAGS resource and give lightning-updater its own "
        "honest metadata another way.")
endif()

# A missing target or object must fail the configure loudly. Silently skipping
# would produce a Windows binary with no version resource at all, which the
# metadata gate would then report as a confusing "unreadable version resource"
# much later in the build.
function(_lightning_attach_version_resource target object)
    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "packaging expected the target '${target}' to exist in the Lightning "
            "source; the Windows version-resource wiring is out of date")
    endif()
    if(NOT EXISTS "${object}")
        message(FATAL_ERROR "version resource object is missing: ${object}")
    endif()
    target_link_options(${target} PRIVATE "${object}")
endfunction()

# Guarded on the inputs rather than on WIN32: only build-windows.sh ever sets
# them, and keying on what was actually supplied makes the file exercisable
# outside a cross-build (tests/test-windows-version-resources.sh).
if(LIGHTNING_APP_VERSION_OBJECT AND LIGHTNING_UPDATER_VERSION_OBJECT)
    cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        CALL _lightning_attach_version_resource
             lightning-matrix "${LIGHTNING_APP_VERSION_OBJECT}")
    cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        CALL _lightning_attach_version_resource
             lightning-updater "${LIGHTNING_UPDATER_VERSION_OBJECT}")
endif()
