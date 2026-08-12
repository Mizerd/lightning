# SignPath Foundation compliance contract.
#
# These are the repository-controlled prerequisites for SignPath Foundation OSS
# code signing. They are checked here rather than merely written down, because a
# policy document that silently stops being linked — or a release version that
# silently disagrees with itself — is exactly the failure this is meant to catch.
#
# Run:  cmake -DSOURCE_ROOT=<repo> -P tests/SignPathComplianceTest.cmake

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

set(_failures 0)

function(_fail message)
    message(SEND_ERROR "FAIL: ${message}")
    math(EXPR _n "${_failures} + 1")
    set(_failures "${_n}" PARENT_SCOPE)
endfunction()

function(_ok message)
    message(STATUS "ok: ${message}")
endfunction()

function(_read_file out path)
    if(NOT EXISTS "${path}")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    file(READ "${path}" _contents)
    set(${out} "${_contents}" PARENT_SCOPE)
endfunction()

# --- 1. The Code signing policy document ------------------------------------

set(_policy "${SOURCE_ROOT}/docs/code-signing-policy.md")
_read_file(_policy_text "${_policy}")
if(_policy_text STREQUAL "")
    _fail("docs/code-signing-policy.md is missing or empty")
else()
    _ok("docs/code-signing-policy.md exists")

    # SignPath requires the exact term on the policy page.
    if(_policy_text MATCHES "# Code signing policy")
        _ok("policy uses the required 'Code signing policy' heading")
    else()
        _fail("docs/code-signing-policy.md must use the heading 'Code signing policy'")
    endif()

    # SignPath requires this attribution verbatim.
    set(_attribution
        "Free code signing provided by SignPath.io, certificate by SignPath Foundation")
    string(FIND "${_policy_text}" "${_attribution}" _pos)
    if(_pos GREATER_EQUAL 0)
        _ok("policy carries the required SignPath attribution verbatim")
    else()
        _fail("docs/code-signing-policy.md must contain, verbatim: ${_attribution}")
    endif()

    # SignPath requires the roles to be named.
    foreach(_role "Authors / committers" "Reviewers" "Approvers")
        string(FIND "${_policy_text}" "${_role}" _rolepos)
        if(_rolepos GREATER_EQUAL 0)
            _ok("policy names the ${_role} role")
        else()
            _fail("docs/code-signing-policy.md must name the '${_role}' role")
        endif()
    endforeach()

    # The policy must point at the privacy policy (SignPath asks for a privacy
    # policy link or an explicit no-transfer statement).
    if(_policy_text MATCHES "privacy\\.md")
        _ok("policy links the privacy policy")
    else()
        _fail("docs/code-signing-policy.md must link docs/privacy.md")
    endif()
endif()

# --- 2. The privacy policy ---------------------------------------------------

_read_file(_privacy_text "${SOURCE_ROOT}/docs/privacy.md")
if(_privacy_text STREQUAL "")
    _fail("docs/privacy.md is missing or empty")
else()
    _ok("docs/privacy.md exists")
endif()

# --- 3. The project home page ------------------------------------------------

_read_file(_readme "${SOURCE_ROOT}/README.md")
if(_readme STREQUAL "")
    _fail("README.md is missing or empty")
else()
    # SignPath requires the term on the project's home page, linked to the
    # policy.
    string(FIND "${_readme}" "Code signing policy" _pos)
    if(_pos GREATER_EQUAL 0)
        _ok("README names 'Code signing policy'")
    else()
        _fail("README.md must contain the term 'Code signing policy'")
    endif()
    if(_readme MATCHES "docs/code-signing-policy\\.md")
        _ok("README links the code signing policy")
    else()
        _fail("README.md must link docs/code-signing-policy.md")
    endif()
    if(_readme MATCHES "docs/privacy\\.md")
        _ok("README links the privacy policy")
    else()
        _fail("README.md must link docs/privacy.md")
    endif()

    # Until a signed release actually ships, the README must keep saying the
    # Windows artifacts are unsigned. This guard is deliberately blunt: a
    # premature "signed" claim is the one documentation error that would
    # mislead a user about a security property.
    if(_readme MATCHES "unsigned")
        _ok("README still discloses that Windows artifacts are unsigned")
    else()
        _fail("README.md no longer discloses unsigned Windows artifacts; "
              "update this test in the same commit as the first signed release")
    endif()
endif()

# --- 4. One canonical release version ---------------------------------------

_read_file(_cmake_text "${SOURCE_ROOT}/CMakeLists.txt")
set(_cmake_version "")
if(_cmake_text MATCHES "VERSION[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)")
    set(_cmake_version "${CMAKE_MATCH_1}")
endif()
set(_label_version "")
if(_cmake_text MATCHES "APP_VERSION_LABEL[ \t]+\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
    set(_label_version "${CMAKE_MATCH_1}")
endif()

_read_file(_cargo_text "${SOURCE_ROOT}/rust/Cargo.toml")
set(_cargo_version "")
if(_cargo_text MATCHES "\nversion[ \t]*=[ \t]*\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
    set(_cargo_version "${CMAKE_MATCH_1}")
endif()

if(_cmake_version STREQUAL "")
    _fail("could not read the project version from CMakeLists.txt")
elseif(NOT _cmake_version STREQUAL _label_version)
    _fail("CMake project version ${_cmake_version} != APP_VERSION_LABEL ${_label_version}")
elseif(NOT _cmake_version STREQUAL _cargo_version)
    _fail("CMake project version ${_cmake_version} != rust/Cargo.toml ${_cargo_version}")
else()
    _ok("one canonical release version everywhere: ${_cmake_version}")
endif()

# The HTTP user agent must be derived from the crate version, never a literal
# that can drift from the released version.
file(GLOB _rust_sources "${SOURCE_ROOT}/rust/src/*.rs")
set(_hardcoded_ua "")
foreach(_rs IN LISTS _rust_sources)
    file(READ "${_rs}" _rs_text)
    if(_rs_text MATCHES "user_agent\\(\"Lightning/[0-9]")
        get_filename_component(_rs_name "${_rs}" NAME)
        list(APPEND _hardcoded_ua "${_rs_name}")
    endif()
endforeach()
if(_hardcoded_ua)
    string(REPLACE ";" ", " _ua_list "${_hardcoded_ua}")
    _fail("hard-coded user-agent version in: ${_ua_list} (use crate::USER_AGENT)")
else()
    _ok("HTTP user agent is derived from the crate version")
endif()

# --- 5. No SignPath secret material is committed ------------------------------
#
# Placeholder NAMES are expected and fine; a value assigned to one is not.
file(GLOB_RECURSE _scan_files
    "${SOURCE_ROOT}/docs/*.md"
    "${SOURCE_ROOT}/scripts/*"
    "${SOURCE_ROOT}/cmake/*"
    "${SOURCE_ROOT}/README.md")
set(_leaks "")
foreach(_file IN LISTS _scan_files)
    if(IS_DIRECTORY "${_file}")
        continue()
    endif()
    file(READ "${_file}" _text)
    if(_text MATCHES "SIGNPATH_[A-Z_]+[ \t]*[:=][ \t]*[\"']?[A-Za-z0-9+/=_-]{16}")
        file(RELATIVE_PATH _rel "${SOURCE_ROOT}" "${_file}")
        list(APPEND _leaks "${_rel}")
    endif()
endforeach()
if(_leaks)
    string(REPLACE ";" ", " _leak_list "${_leaks}")
    _fail("possible committed SignPath secret value in: ${_leak_list}")
else()
    _ok("no SignPath secret values are committed")
endif()

# --- result ------------------------------------------------------------------

if(_failures GREATER 0)
    message(FATAL_ERROR "SignPath compliance: ${_failures} failure(s)")
endif()
message(STATUS "SignPath compliance: all checks passed")
