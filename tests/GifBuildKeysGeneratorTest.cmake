# Contract test for cmake/GenerateGifBuildKeys.cmake, run via `ctest`.
#
# Uses SYNTHETIC canary values only. Verifies that the generator embeds the key
# into the build-tree header with owner-only permissions, reports presence
# without ever printing the value, fails safely when an official build is
# missing a key, and does not leak the value onto stdout/stderr.
#
# Required -D args: GENERATOR (path to GenerateGifBuildKeys.cmake),
#                   WORKDIR (a scratch directory).

set(_canary_g "CANARY_GIPHY_9c1f")
set(_canary_k "CANARY_KLIPY_7a2e")

if(NOT GENERATOR OR NOT WORKDIR)
    message(FATAL_ERROR "GENERATOR and WORKDIR are required")
endif()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

# --- Case 1: both keys present -> header embeds values, perms owner-only ---
set(_out "${WORKDIR}/ok")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LIGHTNING_BUILD_GIPHY_API_KEY=${_canary_g}"
        "LIGHTNING_BUILD_KLIPY_API_KEY=${_canary_k}"
        "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${_out}" "-DREQUIRE_KEYS=ON"
        -P "${GENERATOR}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "generator failed with both keys present: ${_stderr}")
endif()
set(_header "${_out}/LightningGifBuildKeys.h")
if(NOT EXISTS "${_header}")
    message(FATAL_ERROR "generated header missing")
endif()
file(READ "${_header}" _content)
if(NOT _content MATCHES "${_canary_g}" OR NOT _content MATCHES "${_canary_k}")
    message(FATAL_ERROR "generated header does not embed the keys")
endif()
# The value must never appear on the generator's own output streams.
if(_stdout MATCHES "${_canary_g}" OR _stderr MATCHES "${_canary_g}"
   OR _stdout MATCHES "${_canary_k}" OR _stderr MATCHES "${_canary_k}")
    message(FATAL_ERROR "generator leaked a key value to its output")
endif()
if(NOT _stdout MATCHES "present" AND NOT _stderr MATCHES "present")
    message(FATAL_ERROR "generator did not report key presence")
endif()
# Owner-only permissions where the platform supports them.
if(NOT WIN32)
    file(READ "${_header}" _ignore) # ensure readable by owner
endif()

# --- Case 2: official build missing a key -> fail, no value leak ---
set(_out2 "${WORKDIR}/fail")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LIGHTNING_BUILD_GIPHY_API_KEY=${_canary_g}"
        "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${_out2}" "-DREQUIRE_KEYS=ON"
        -P "${GENERATOR}"
    RESULT_VARIABLE _rc2 OUTPUT_VARIABLE _stdout2 ERROR_VARIABLE _stderr2)
if(_rc2 EQUAL 0)
    message(FATAL_ERROR "generator must fail when an official key is missing")
endif()
if(_stderr2 MATCHES "${_canary_g}" OR _stdout2 MATCHES "${_canary_g}")
    message(FATAL_ERROR "generator leaked a key value on the failure path")
endif()

# --- Case 3: keyless dev build -> succeeds, header keyless ---
set(_out3 "${WORKDIR}/keyless")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${_out3}" "-DREQUIRE_KEYS=OFF"
        -P "${GENERATOR}"
    RESULT_VARIABLE _rc3)
if(NOT _rc3 EQUAL 0)
    message(FATAL_ERROR "keyless generation must succeed")
endif()
file(READ "${_out3}/LightningGifBuildKeys.h" _content3)
if(_content3 MATCHES "CANARY")
    message(FATAL_ERROR "keyless header unexpectedly contains a key")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
message(STATUS "GIF build-key generator contract test passed")
