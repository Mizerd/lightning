# Per-target Windows version resources for the Lightning cross-build.
#
# Injected with -DCMAKE_PROJECT_INCLUDE=<this file>, which CMake includes right
# after the top-level project(); the application source is not patched.
#
# Each executable needs its own resource: a global CMAKE_EXE_LINKER_FLAGS
# resource would stamp lightning-updater.exe with OriginalFilename
# "Lightning.exe", which verify-windows-metadata.py rejects. CMake cannot set
# per-target link options from the command line, so the assignment is deferred
# to the end of the top-level directory scope, when both targets exist.
#
# Inputs (set by scripts/build-windows.sh):
#   LIGHTNING_APP_VERSION_OBJECT      windres object for lightning-matrix
#   LIGHTNING_UPDATER_VERSION_OBJECT  windres object for lightning-updater

if(CMAKE_VERSION VERSION_LESS 3.19)
    message(FATAL_ERROR
        "cmake_language(DEFER) needs CMake 3.19+; the Windows builder image has "
        "${CMAKE_VERSION}. Update the image or restore a global "
        "CMAKE_EXE_LINKER_FLAGS resource and give lightning-updater its own "
        "honest metadata another way.")
endif()

# A missing target or object fails the configure; skipping would ship a binary
# with no version resource.
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

# Keyed on the inputs rather than WIN32, so the file can be tested outside a
# cross-build (tests/test-windows-version-resources.sh).
if(LIGHTNING_APP_VERSION_OBJECT AND LIGHTNING_UPDATER_VERSION_OBJECT)
    cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        CALL _lightning_attach_version_resource
             lightning-matrix "${LIGHTNING_APP_VERSION_OBJECT}")
    cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        CALL _lightning_attach_version_resource
             lightning-updater "${LIGHTNING_UPDATER_VERSION_OBJECT}")
endif()
